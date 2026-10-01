"""Gaze first, the relay's part (docs/gaze-first.md).

While the pointer helper says gaze first is on ("gazefirst 1" every 5 s: gaze mode, no game,
the headset on; "gazefirst 0" or 12 s of silence ends it), the Frame controllers belong to the
gaze:

- SteamVR: the Frame controller's compositor binding is
  pointer/bindings/vrcompositor_frame_controller_gazefirst.json (only the Steam button left),
  chosen through vrserver's /input/selectconfig.action, and the stock one again after.
- Steam's UI: input/steam-gamepad-filter.js runs in Steam's SharedJSContext (input/steamui.py)
  and drops the controllers' gamepad input (the Steam button still passes). The block lasts 15 s
  unless it's renewed, which happens every 5 s, so it ends by itself if the relay dies; and
  Steam reloads its UI with SteamVR, which the renewal also covers.
- The trigger and bumper, read from vrserver's web socket (input/vrws.py, which takes nothing
  from anyone), go to the helper as "ctrl <left|right> <trigger|bumper> 1|0"; the right
  thumbstick scrolls ("scroll <x> <y>", as the mouse's wheel).

Steam still sees the controllers, and SteamVR leaves laser mode after each press and release:
the helper takes the laser back (see "Gaze first" in pointer/helper/ft-pointer.cpp).

The toggle macro works with gaze first on or off: both thumbstick clicks held 1 s turn gaze mode
on or off, and POINTER_GAZE in ~/.config/frametop.conf remembers it. So does `toggle()`, which
the relay's gaze_toggle action uses.

The binding, the filter, and finding new controllers run on a thread of their own: they're HTTP
and web socket round trips that mustn't hold up the relay's mouse and keyboard.
"""
import json
import os
import threading
import time
import urllib.request

import steamui
from vrws import HEADERS, ORIGIN, VrSocket, getstate

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILTER_JS = os.path.join(ROOT, "input", "steam-gamepad-filter.js")
GAZE_BINDING = "file://" + os.path.join(ROOT, "pointer", "bindings", "vrcompositor_frame_controller_gazefirst.json")
STOCK_BINDING = "file:///opt/steamvr/drivers/frame_controller/resources/input/vrcompositor_bindings_frame_controller.json"
COMPOSITOR = "openvr.component.vrcompositor"
CONF = os.path.expanduser("~/.config/frametop.conf")

BUTTONS = {"/input/trigger/click": "trigger", "/input/bumper/click": "bumper"}
STICK_CLICK = "/input/thumbstick/click"
MACRO_HOLD = 1.0      # seconds both thumbstick clicks are held to toggle gaze mode
SCROLL_DEADZONE = 0.3
ON_FOR = 12.0         # "gazefirst 1" lasts this long without another
RENEW = 5.0           # the binding and filter are checked (and the filter's block renewed) this often
BLOCK_FOR = 15000     # ms the filter blocks without a renewal


def select_binding(url):
    body = json.dumps({"app_key": COMPOSITOR, "controller_type": "frame_controller", "url": url}).encode()
    req = urllib.request.Request(f"{ORIGIN}/input/selectconfig.action", data=body,
                                 headers={**HEADERS, "Origin": ORIGIN, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=2) as resp:
        if not json.load(resp).get("success"):
            raise OSError("vrserver refused the binding")


def current_binding():
    req = urllib.request.Request(f"{ORIGIN}/input/getactions.json?app_key={COMPOSITOR}", headers=HEADERS)
    with urllib.request.urlopen(req, timeout=2) as resp:
        return (json.load(resp).get("current_binding_url") or {}).get("frame_controller", "")


def read_gaze(path=CONF):
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line.split("=", 1)[0].strip() == "POINTER_GAZE" and "=" in line:
                    return line.split("=", 1)[1].strip() not in ("", "0")
    except OSError:
        pass
    return False


def write_gaze(on, path=CONF):
    """POINTER_GAZE=1|0 in the config, keeping every other line as it is."""
    try:
        with open(path) as f:
            lines = f.read().splitlines()
    except OSError:
        lines = []
    value = f"POINTER_GAZE={1 if on else 0}"
    for i, line in enumerate(lines):
        if line.split("#", 1)[0].split("=", 1)[0].strip() == "POINTER_GAZE":
            lines[i] = value
            break
    else:
        lines.append(value)
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write("\n".join(lines) + "\n")
    os.replace(tmp, path)


class GazeFirst:
    def __init__(self, send, log):
        self.send = send  # a command to the pointer helper
        self.log = log
        self.on = False
        self.on_until = 0.0
        self.ws = None
        self.next_connect = 0.0
        self.sides = {}          # subscribed device path -> "left" or "right"
        self.values = {}         # (side, component) -> last value
        self.held = set()        # (side, button) the helper has as pressed
        self.macro_since = None
        self.macro_fired = False
        self.scrolling = (0.0, 0.0)
        # The worker thread's: a wake-up, the devices it found, what it last applied.
        self.lock = threading.Lock()
        self.wake = threading.Event()
        self.found = []
        self.applied = None
        self.problems = {}
        threading.Thread(target=self.work, daemon=True, name="gazefirst").start()

    # The relay's select loop: readable while connected to vrserver.
    def fileno(self):
        return self.ws.fileno()

    def message(self, words, now):
        """"gazefirst 1|0" from the helper."""
        on = words[1] == "1"
        self.on_until = now + ON_FOR if on else 0.0
        if on != self.on:
            self.set_on(on)

    def set_on(self, on):
        self.on = on
        if not on:
            self.release_all(cancel=True)
        self.log(f"gaze first {'on' if on else 'off'}")
        self.wake.set()

    def release_all(self, cancel=False):
        """Let go of what the helper holds: as releases (a click), or cancelled (no click)."""
        if cancel and self.held:
            self.send("ctrlcancel")
        else:
            for side, button in sorted(self.held):
                self.send(f"ctrl {side} {button} 0")
        self.held.clear()
        if self.scrolling != (0.0, 0.0):
            self.send("scroll 0 0")
            self.scrolling = (0.0, 0.0)

    def toggle(self):
        """Gaze mode on or off, remembered (POINTER_GAZE)."""
        on = not read_gaze()
        try:
            write_gaze(on)
        except OSError as e:
            self.log(f"gaze mode: can't write {CONF}: {e}")
        self.send(f"gaze {'on' if on else 'off'}")
        self.log(f"gaze mode {'on' if on else 'off'}")

    def timeout(self):
        return 0.05 if self.macro_since is not None and not self.macro_fired else 0.5

    def tick(self, now):
        if self.on and now > self.on_until:
            self.set_on(False)  # the helper went quiet
        if self.ws is None and now >= self.next_connect:
            self.next_connect = now + 3.0
            try:
                self.ws = VrSocket(timeout=1)
                self.ws.open(f"frametop_relay_{os.getpid()}")
                self.sides = {}
                self.wake.set()  # find the controllers now
            except OSError:
                self.ws = None
        with self.lock:
            found, self.found = self.found, []
        for path, side in found:
            if self.ws and self.sides.get(path) != side:
                try:
                    self.ws.subscribe(path)
                    self.sides[path] = side
                except OSError:
                    self.drop()
        if self.macro_since is not None and not self.macro_fired and now - self.macro_since >= MACRO_HOLD:
            self.macro_fired = True
            self.release_all(cancel=True)  # a press in progress doesn't click
            self.toggle()

    def drop(self):
        if self.ws:
            try:
                self.ws.close()
            except OSError:
                pass
        self.ws = None
        self.release_all(cancel=True)

    def readable(self, now):
        try:
            while True:
                msg = self.ws.recv(timeout=0)
                if isinstance(msg, dict) and msg.get("type") == "update_component_states":
                    side = self.sides.get(msg.get("device"))
                    if side:
                        self.update(side, msg.get("components") or {}, now)
                if not self.ws.pending():
                    return  # the rest, if any, wakes select again
        except OSError:
            self.drop()

    def update(self, side, components, now):
        for name, value in components.items():
            key = (side, name)
            if self.values.get(key) == value:
                continue
            self.values[key] = value
            button = BUTTONS.get(name)
            if button:
                down = bool(value)
                if down and self.on and (side, button) not in self.held:
                    self.held.add((side, button))
                    self.send(f"ctrl {side} {button} 1")
                elif not down and (side, button) in self.held:
                    self.held.discard((side, button))
                    self.send(f"ctrl {side} {button} 0")
            elif name == STICK_CLICK:
                both = self.values.get(("left", STICK_CLICK)) and self.values.get(("right", STICK_CLICK))
                if both and self.macro_since is None:
                    self.macro_since, self.macro_fired = now, False
                elif not both:
                    self.macro_since = None
            elif side == "right" and name in ("/input/thumbstick/x", "/input/thumbstick/y"):
                self.scroll()

    def scroll(self):
        def shape(v):
            v = float(v or 0)
            if abs(v) < SCROLL_DEADZONE:
                return 0.0
            return round((abs(v) - SCROLL_DEADZONE) / (1 - SCROLL_DEADZONE) * (1 if v > 0 else -1), 1)
        want = (shape(self.values.get(("right", "/input/thumbstick/x"))),
                shape(self.values.get(("right", "/input/thumbstick/y")))) if self.on else (0.0, 0.0)
        if want != self.scrolling:
            self.scrolling = want
            self.send(f"scroll {want[0]:.1f} {want[1]:.1f}")

    def shutdown(self):
        """The relay is stopping: the controllers go back to SteamVR and Steam."""
        self.on = False
        self.release_all(cancel=True)
        try:
            select_binding(STOCK_BINDING)
        except OSError:
            pass
        try:
            steamui.evaluate('window.__frametopGaze && (window.__frametopGaze.mode = "off")', timeout=1)
        except (OSError, RuntimeError):
            pass

    # The worker thread.
    def problem(self, what, error=None):
        """Log a failure once, and when it's over (error None)."""
        if error is None:
            if self.problems.pop(what, None) is not None:
                self.log(f"gaze first: {what} works again")
            return
        if self.problems.get(what) != str(error):
            self.problems[what] = str(error)
            self.log(f"gaze first: {what}: {error}")

    def work(self):
        while True:
            self.wake.wait(RENEW)
            self.wake.clear()
            on = self.on
            try:
                found = [(d["root_path"], d.get("side") or d["root_path"].rsplit("/", 1)[-1])
                         for d in getstate(timeout=1) if d.get("controller_type") == "frame_controller"]
                with self.lock:
                    self.found = [(p, s) for p, s in found if s in ("left", "right")]
                self.problem("vrserver")
            except (OSError, ValueError) as e:
                self.problem("vrserver", e)
                continue
            try:
                want = GAZE_BINDING if on else STOCK_BINDING
                if current_binding() != want:
                    select_binding(want)
                    self.log(f"gaze first: {'gaze' if on else 'stock'} controller binding")
                self.problem("binding")
            except (OSError, ValueError) as e:
                self.problem("binding", e)
            try:
                if on:
                    with open(FILTER_JS) as f:
                        steamui.evaluate(f.read())
                    steamui.evaluate('(() => { const G = window.__frametopGaze; G.mode = "block"; '
                                     f'G.until = Date.now() + {BLOCK_FOR}; return G.mode; }})()')
                elif self.applied is not False:
                    steamui.evaluate('window.__frametopGaze && (window.__frametopGaze.mode = "off")')
                self.applied = on
                self.problem("Steam's UI")
            except (OSError, RuntimeError, ValueError) as e:
                self.problem("Steam's UI", e)
