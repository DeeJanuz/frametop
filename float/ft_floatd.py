#!/usr/bin/env python3
"""ft-floatd: floating windows for the Frametop desktop (see docs/floating-windows.md).

Runs inside the desktop's Plasma session (its D-Bus and Wayland). It keeps the table of
which window floats on which spare output and panel, and connects three parts:
  - the KWin script frametop-float (float/frametop-float.js), which it loads into KWin. The
    script sends events over D-Bus (org.frametop.Float.Event) and fetches commands with a
    long poll (NextCommand).
  - ft-screens, through its control socket (@ft_screens): the spare output's size, and the
    floating panel's crop, density, place, and popups.
  - kscreen-doctor, to turn spare outputs on and off and place them in KWin's layout.
Commands and ft-screens' events arrive as datagrams on @frametop_float (ft-float is the
command-line side). Replies go to the sender:
  float [ID|active|pointer]   dock [ID|active|all]   close ID   list   quit   (ft-float)
  launch APP.desktop   run ["argv", ...]   (start an app and float its first window)
  windows   (-> JSON: the open apps' windows and where they are, for a profile; ft-layout save)
  profile NAME   (open a profile's apps: move the matching windows, launch the missing ones)
  ("float pointer" is the float key: the window under the pointer, else the active one,
  floated or docked; the input relay sends it for float_toggle, and "dock all" for dock_all)
  dock N | close N | resize N W H | scale N STEPS                      (ft-screens, N = its screen)

Spare outputs are WL-<screens> .. WL-<screens + slots - 1>. A floating window's output is its
frame plus a margin on each side (FLOAT_MARGIN pixels), so menus have room; the panel shows
only the window. Spares are placed apart from the screens and from each other in KWin's
layout, all within Xwayland's 32767-pixel limit.

Usage: ft-floatd [--screens N] [--slots N] [--margin PX] [--control NAME] [--socket NAME]
Defaults: FT_SCREEN_COUNT (from the session) or the layout's count, FLOAT_SLOTS and
FLOAT_MARGIN from ~/.config/frametop.conf (8 and 300), @ft_screens, @frametop_float.

Launching floating: "launch" starts a desktop file's app (Gio), "run" a command. The next
normal window from that process (or a child), or with that desktop file name, within
LAUNCH_SECONDS floats: where that app last floated, or in front of you. Single-instance and
D-Bus-activated apps open their window from a process that was already running, hence the
name. Each app's last floating place (its pose relative to the primary screen, so it moves
with the screens' layout), size in pixels, and scale are kept in PLACES_PATH, by desktop
file name, whenever one of its windows stops floating.

Profiles (docs/profiles.md): "windows" lists every app window with where it is: on a screen
(its rectangle on that output, maximized or not) or floating (its pose relative to the primary
screen, size, scale). "profile NAME" takes a profile's windows from the layout file: the
windows of each app already open (oldest first) move to its entries, and for the entries
left the app is launched once, and again for each window still missing 3 seconds after its
first one shows up (a browser restores its own windows; a terminal opens one each time).
Nothing is ever closed.

Launch as Standalone (float/ft_apps.py): copies of the apps' desktop files with that action,
which only the Frametop desktop reads. ft-floatd rewrites them when apps change.
"""
import argparse
import json
import math
import os
import re
import socket
import subprocess
import sys
import time

import dbus
import dbus.mainloop.glib
import dbus.service
from gi.repository import Gio, GLib

HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "layout"))
import ft_layout  # noqa: E402  (config and layout)
import ft_apps  # noqa: E402  (Launch as Standalone)

SERVICE = IFACE = "org.frametop.Float"
PATH = "/Float"
SCRIPT = "frametop-float"
POLL_SECONDS = 20        # NextCommand answers empty after this (KWin's D-Bus timeout is 25 s)
SPARE_X, SPARE_CELL = 12000, 5000  # spares in KWin's layout: a grid from here, 4 across
PULL_OUT = 0.3           # a floated window starts this far in front of its screen (metres), clear of it
DEFAULT_MPP = 1.6 / 1920  # metres per pixel when ft-screens can't say (no SteamVR)
PLACES_PATH = os.path.expanduser("~/.config/frametop-float.json")  # each app's last floating place
LAUNCH_SECONDS = 30      # a launched app's window must show up within this
IN_FRONT = 2.0           # at most this far in front of you, for an app with no remembered place
DEBUG = os.environ.get("FT_FLOAT_DEBUG") == "1"  # log every event from the script


def whole(*scales):
    """The multiple a spare's size in pixels must be at these scales: KWin's nested backend gives
    the buffer a whole buffer scale (1.2 -> 2), and a buffer that isn't a multiple of it is a
    protocol error that disconnects KWin."""
    k = 1
    for s in scales:
        k = math.lcm(k, max(1, math.ceil(s - 1e-6)))
    return k


def kwin_size(px, scale, k):
    """What to ask ft-screens for so a spare comes out at least px pixels, a multiple of k. KWin
    makes a nested output the size it's configured to times its scale, rounded. Returns (the
    size to ask for, the pixels it comes out); the margin takes the extra pixels."""
    n = max(1, math.floor(px / scale))
    while True:
        exact = n * scale
        p = math.floor(exact + 0.5)
        if p >= px and p % k == 0 and abs(exact - math.floor(exact) - 0.5) > 1e-6:
            return n, p
        n += 1


def log(*args):
    print(time.strftime("%H:%M:%S"), *args, flush=True)


class Screens:
    """ft-screens' control socket."""

    def __init__(self, name):
        self.address = "\0" + name
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("")

    def ask(self, text, quiet=False):
        self.sock.settimeout(2.0)
        try:
            self.sock.sendto(text.encode(), self.address)
            reply = self.sock.recv(8192).decode()
        except OSError as e:
            reply = f"error no answer ({e})"
        if not reply.startswith("ok") and not quiet:
            log(f"ft-screens: {text}: {reply}")
        return reply


def kscreen(*args):
    try:
        r = subprocess.run(["kscreen-doctor", *args], capture_output=True, text=True, timeout=20)
        return r.stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        log(f"kscreen-doctor: {e}")
        return ""


def output_rects():
    """Each output's place and size in KWin's layout (logical)."""
    try:
        data = json.loads(kscreen("-j") or "{}")
    except ValueError:
        return {}
    out = {}
    for o in data.get("outputs", []):
        pos, size, scale = o.get("pos") or {}, o.get("size") or {}, float(o.get("scale", 1)) or 1.0
        if o.get("name") and size:
            out[o["name"]] = (pos.get("x", 0), pos.get("y", 0), size.get("width", 0) / scale, size.get("height", 0) / scale)
    return out


def output_scales():
    try:
        data = json.loads(kscreen("-j") or "{}")
    except ValueError:
        return {}
    return {o["name"]: float(o.get("scale", 1)) for o in data.get("outputs", []) if o.get("name")}


def load_places():
    try:
        with open(PLACES_PATH) as f:
            places = json.load(f)
        return places if isinstance(places, dict) else {}
    except (OSError, ValueError):
        return {}


def save_places(places):
    tmp = PLACES_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(places, f, indent=1)
    os.replace(tmp, PLACES_PATH)


def to_local(ref, g):
    """A panel's pose (ft_layout.parse_get) in the frame of another panel (ref): its centre
    and axes, 12 numbers."""
    axes = (ref["x"], ref["y"], ref["z"])
    d = [g["center"][k] - ref["center"][k] for k in range(3)]
    out = [ft_layout.dot(d, a) for a in axes]
    for v in (g["x"], g["y"], g["z"]):
        out += [ft_layout.dot(v, a) for a in axes]
    return [round(v, 5) for v in out]


def to_world(ref, local):
    """to_local's inverse: (centre, x, y, z) in the world."""
    axes = (ref["x"], ref["y"], ref["z"])

    def turn(v):
        return [sum(v[i] * axes[i][k] for i in range(3)) for k in range(3)]
    c = turn(local[0:3])
    c = [c[k] + ref["center"][k] for k in range(3)]
    return c, turn(local[3:6]), turn(local[6:9]), turn(local[9:12])


def pid_chain(pid):
    """A process and its ancestors, nearest first."""
    out = []
    while pid > 1 and len(out) < 32:
        out.append(pid)
        try:
            with open(f"/proc/{pid}/stat") as f:
                pid = int(f.read().rsplit(")", 1)[1].split()[1])
        except (OSError, ValueError, IndexError):
            break
    return out


# Windows a profile doesn't keep: Plasma's own, the Frametop settings apps, the login splash.
SKIP_APPS = ("org.kde.plasmashell", "org.kde.krunner", "org.kde.ksplashqml", "org.kde.polkit-kde-authentication-agent-1")


def recordable(ev):
    """An app's top-level window, the kind a profile keeps."""
    if not ev.get("normal") or ev.get("popup") or ev.get("transient"):
        return False
    app, cls = ev.get("app", ""), ev.get("cls", "")
    return app not in SKIP_APPS and not any(n.startswith(("ft-", "frametop", "ksplash")) for n in (app, cls))


def cmdline(pid):
    try:
        with open(f"/proc/{int(pid)}/cmdline", "rb") as f:
            return [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
    except (OSError, ValueError, TypeError):
        return []


class Launch:
    """An app we started: its first window floats, or (for a profile) its windows go to the
    profile's entries for it, in order."""

    def __init__(self, app, pid, entries=None, argv=None):
        self.app = app            # its desktop file name, without .desktop ("" for a command)
        self.argv = argv          # the command, when it isn't a desktop file's app
        self.pids = [pid] if pid else []  # the processes we started (none when D-Bus started it)
        self.entries = entries    # a profile's entries still waiting for a window, or None
        self.relaunched = False
        self.until = time.monotonic() + LAUNCH_SECONDS


class Float:
    """A floating window."""

    def __init__(self, wid, slot, saved):
        self.id = wid
        self.slot = slot          # Slot
        self.saved = saved        # where it came from: output, frame, onAllDesktops
        self.scale = 1.0          # its output's scale (pixels per logical unit)
        self.mpp = DEFAULT_MPP    # metres per pixel on its panel
        self.frame = None         # last frame (logical, global)
        self.client = None
        self.full = False         # full screen: no margin
        self.normal = None        # its size in pixels when last not full screen
        self.unfull_until = 0.0   # left full screen just now (see follow)
        self.move_from = None     # frame when a title-bar move started (put back after)
        self.resizing = False     # KWin's resize by the window's edge is going on (see follow)
        self.subs = {}            # popup or dialog id -> number on the panel
        self.app = ""             # its desktop file name (for its remembered place)


class Slot:
    def __init__(self, k, screens):
        self.k = k
        self.output = f"WL-{screens + k}"
        self.index = screens + k + 1  # ft-screens' number (1-based)
        self.pos = (SPARE_X + (k % 4) * SPARE_CELL, (k // 4) * SPARE_CELL)
        self.size = None              # its output's size in pixels, as last set
        self.want = None              # the size in pixels asked for (size is at least that)
        self.kscale = 1.0             # its output's scale in KWin, as last set
        self.window = None            # Float


class Daemon:
    def __init__(self, args):
        self.screens_n = args.screens
        self.margin = args.margin
        self.slots = [Slot(k, args.screens) for k in range(args.slots)]
        self.floats = {}         # window id -> Float
        self.windows = {}        # window id -> last info from the script
        self.gone = set()        # ids of windows that closed (KWin's ids aren't reused)
        self.pending = []        # commands for the script
        self.waiter = None       # (reply callback, timeout source) while the script waits
        self.screens = Screens(args.control)
        self.sub_numbers = {}    # popup/dialog id -> (window id, number)
        self.next_sub = 1
        self.launches = []       # Launch: apps we started, waiting for their window
        self.captures = {}       # token -> sender: "windows" requests waiting for the script's report
        self.next_token = 1
        self.sock = None         # our socket (set by main), for deferred replies
        self.places = load_places()  # app -> {"rel": pose by the primary screen, "pixels", "scale"}

    # ------------------------------------------------------------ the script

    def command(self, **cmd):
        self.pending.append(cmd)
        self.flush()

    def flush(self):
        if not self.waiter or not self.pending:
            return
        reply, source = self.waiter
        self.waiter = None
        GLib.source_remove(source)
        text, self.pending = json.dumps(self.pending), []
        reply(text)

    def wait(self, reply):
        if self.waiter:  # a stale poll (the script reloaded): let it go
            old, source = self.waiter
            GLib.source_remove(source)
            old("")

        def timeout():
            if self.waiter and self.waiter[0] is reply:
                self.waiter = None
                reply("")
            return False
        self.waiter = (reply, GLib.timeout_add_seconds(POLL_SECONDS, timeout))
        self.flush()

    def load_script(self, bus):
        kwin = dbus.Interface(bus.get_object("org.kde.KWin", "/Scripting"), "org.kde.kwin.Scripting")
        if kwin.isScriptLoaded(SCRIPT):
            kwin.unloadScript(SCRIPT)
        sid = int(kwin.loadScript(os.path.join(HERE, "frametop-float.js"), SCRIPT, signature="ss"))
        if sid < 0:
            raise RuntimeError("KWin didn't load the script")
        # Scripting.start runs every loaded script that isn't running. Not /Scripting/Script<id>'s
        # run: a reloaded script gets the old one's id while the old one is still being deleted,
        # so that path is still the old script's, and the new one would never run.
        kwin.start()
        log(f"script loaded ({sid})")
        # Older scripts registered the float key with KWin; the input relay owns it now. Drop
        # that shortcut, so System Settings doesn't list one that does nothing.
        try:
            accel = dbus.Interface(bus.get_object("org.kde.kglobalaccel", "/kglobalaccel"), "org.kde.KGlobalAccel")
            if accel.unregister("kwin", "Frametop Float Window"):
                log("dropped KWin's old float shortcut")
        except dbus.DBusException:
            pass

    # ------------------------------------------------------------ events from the script

    def on_event(self, ev):
        kind = ev.get("ev")
        if DEBUG:
            log("event", {k: v for k, v in ev.items() if k in ("ev", "id", "output", "frame", "fullScreen", "popup",
                                                                "parent", "move")})
        wid = ev.get("id", "")
        if kind == "hello":
            self.command(cmd="config", screens=self.screens_n)
            # The script reports every window after "config"; spares nothing floats on are off.
            GLib.timeout_add(1500, self.disable_unused)
            return
        if wid in self.gone:
            return  # an event that came after the window closed (it would bring it back)
        if kind == "removed":
            self.gone.add(wid)
            self.windows.pop(wid, None)
            if wid in self.floats:
                log(f"{wid[:9]} closed")
                self.release(self.floats.pop(wid))
            self.drop_sub(wid)
            return
        if wid:
            self.windows[wid] = ev
        f = self.floats.get(wid)
        if kind == "reported":
            self.captured(ev.get("token"))
            return
        if kind == "added" and self.launches and not f:
            launch = self.launched(ev)
            if launch and launch.entries is None:
                self.float_launched(ev, launch)
                return
            if launch:
                self.place_entry(ev, launch.entries.pop(0))
                if launch.entries and not launch.relaunched:
                    GLib.timeout_add_seconds(3, lambda: self.relaunch(launch) and False)
                return
        if kind == "added" and not f and self.on_hidden_screen(ev):
            # Nobody would see it there (a profile can hide every screen): it floats instead.
            log(f"{wid[:9]} ({ev.get('cls')}) opened on a hidden screen")
            self.float_launched(ev, None)
            return
        if kind == "float-request":
            self.float_window(ev)
        elif kind == "dock-request":
            if f:
                self.dock(f)
        elif kind in ("added", "window", "output"):
            self.seen(ev)
        elif kind == "geometry":
            if f:
                self.follow(f, ev)
            else:
                self.sub(ev)
        elif kind == "move-start" and f and ev.get("move"):
            f.move_from = ev["frame"]
            self.screens.ask(f"carry {f.slot.index}", quiet=True)
        elif kind == "move-start" and f and ev.get("resize"):
            f.resizing = True
        elif kind == "move-end" and f and f.resizing:
            f.resizing = False
            self.follow(f, ev)
        elif kind == "move-end" and f and f.move_from:
            # The panel carried the window; KWin may have slipped it a few pixels first.
            m, f.move_from = f.move_from, None
            if (m["x"], m["y"]) != (ev["frame"]["x"], ev["frame"]["y"]):
                self.command(cmd="geometry", id=f.id, x=m["x"], y=m["y"], w=ev["frame"]["w"], h=ev["frame"]["h"])
        elif kind == "fullscreen" and f:
            if not ev.get("fullScreen"):
                f.unfull_until = time.monotonic() + 1.0
            self.follow(f, ev)
        elif kind == "minimized" and f:
            self.screens.ask(f"minimized {f.slot.index} {1 if ev.get('minimized') else 0}", quiet=True)

    def spare_slot(self, output):
        for s in self.slots:
            if s.output == output:
                return s
        return None

    def seen(self, ev):
        """A window the script told us about: is it somewhere it shouldn't be?"""
        wid = ev["id"]
        slot = self.spare_slot(ev.get("output", ""))
        f = self.floats.get(wid)
        if f and f.slot is not slot and ev["ev"] == "output":
            # Left its spare (KWin moved it, or docking): it's not floating any more.
            if slot is None:
                log(f"{wid[:9]} left its floating output")
                del self.floats[wid]
                self.release(f)
            return
        if slot is None or f:
            return
        if ev.get("popup") or (ev.get("transient") and ev.get("parent") in self.floats):
            self.sub(ev)
            return
        if not ev.get("normal"):
            return
        if slot.window is None:
            if ev.get("cls") == "ksplashqml":  # the login splash, on every output at first
                return
            # Floating when ft-floatd (re)started: take it over where it is.
            f = Float(wid, slot, None)
            f.app = ev.get("app", "")
            slot.window = f
            self.floats[wid] = f
            f.scale = slot.kscale = output_scales().get(slot.output, 1.0)
            # Its panel's density as it is (follow below sets the panel again).
            g, px = self.panel_get(slot.index), round(ev["frame"]["w"] * f.scale)
            if g and g["metres"] > 0 and px > 0:
                f.mpp = g["metres"] / px
            log(f"{wid[:9]} ({ev.get('cls')}) already floats on {slot.output}")
            self.follow(f, ev)
            return
        # A new window that opened on a floating window's output: windows of floating apps
        # float too; anything else goes to the screens.
        if ev["ev"] == "added" and any(o.saved is not None and self.windows.get(o.id, {}).get("pid") == ev.get("pid")
                                       for o in self.floats.values()):
            self.float_window(ev)
        else:
            # Onto the first screen that shows (or floating, if none does).
            hidden = self.concealed()
            shown = [i for i in range(self.screens_n) if i + 1 not in hidden]
            if not shown:
                self.float_launched(ev, None)
                return
            name = f"WL-{shown[0]}"
            x0, y0, _, _ = output_rects().get(name, (0, 0, 0, 0))
            self.command(cmd="place", id=wid, output=name, x=x0 + ev["frame"]["x"] % 400 + 100,
                         y=y0 + ev["frame"]["y"] % 300 + 100, w=ev["frame"]["w"], h=ev["frame"]["h"])

    # ------------------------------------------------------------ floating and docking

    def sized(self, slot, size, timeout=1.0):
        """Wait (briefly) until KWin has taken the spare's new size, before a scale that needs it."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for line in self.screens.ask("toplevels", quiet=True).splitlines()[1:]:
                f = line.split()
                if len(f) >= 2 and f[0] == str(slot.index) and f[1] == f"{size[0]}x{size[1]}":
                    return True
            time.sleep(0.03)
        log(f"{slot.output} didn't take {size[0]}x{size[1]} in time")
        return False

    def set_size(self, slot, want, k=None):
        """Size a spare's output to at least want (pixels) at its current scale in KWin."""
        if want == slot.want:
            return
        k = k or whole(slot.kscale)
        (w, pw), (h, ph) = kwin_size(want[0], slot.kscale, k), kwin_size(want[1], slot.kscale, k)
        slot.want, slot.size = want, (pw, ph)
        self.screens.ask(f"size {slot.index} {w} {h}")

    def disable_unused(self):
        off = [f"output.{s.output}.disable" for s in self.slots if s.window is None]
        if off:
            kscreen(*off)
        return False

    def free_slot(self):
        for s in self.slots:
            if s.window is None:
                return s
        return None

    def screen_mpp(self, output):
        """Metres per pixel on the screen showing this output, from ft-screens."""
        m = re.match(r"WL-(\d+)$", output or "")
        reply = self.screens.ask("screens", quiet=True)
        if m and reply.startswith("ok"):
            for part in reply.split()[2:]:
                idx, size, metres = part.split(":")
                if int(idx) == int(m.group(1)) + 1:
                    return float(metres) / max(1, int(size.split("x")[0]))
        return DEFAULT_MPP

    def float_window(self, ev, place=None, mpp=None, scale=None):
        """Float a window: its panel in front of where it was on its screen, or at place (centre
        and axes in the world), at the density of its screen, or mpp, and at its screen's scale,
        or scale (then ev's frame is the window's size at that scale). Returns its Float."""
        wid = ev["id"]
        if wid in self.floats:
            return self.floats[wid]
        slot = self.free_slot()
        if slot is None:
            self.command(cmd="mark", id=wid)  # the title bar button set keep-below for nothing
            self.notify(f"All {len(self.slots)} floating windows are in use. Put one back on the desktop "
                        "to float another.")
            return None
        f = Float(wid, slot, {"output": ev["output"], "frame": ev["frame"], "onAllDesktops": ev.get("onAllDesktops")})
        f.app = ev.get("app", "")
        slot.window = f
        self.floats[wid] = f
        scales = output_scales()
        f.scale = scale or scales.get(ev["output"], 1.0)
        slot.kscale = scales.get(slot.output, slot.kscale)
        f.mpp = mpp or self.screen_mpp(ev["output"])
        fr, s, m = ev["frame"], f.scale, self.margin
        w, h = round(fr["w"] * s), round(fr["h"] * s)
        log(f"{wid[:9]} ({ev.get('cls')}) floats on {slot.output}: {w}x{h} px, scale {s:g}")
        # The spare's size first (while it's off, so its first frame is right; it's sized at
        # its old scale, for pixels that suit the new one), then its panel, then turn it on,
        # then the window.
        slot.want = None
        self.set_size(slot, (w + 2 * m, h + 2 * m), whole(slot.kscale, s))
        self.screens.ask(f"scale {slot.index} {s:g}")  # for pointer positions (KWin's units)
        self.set_panel(f, (m, m, w, h), title=round((ev["client"]["y"] - fr["y"]) * s))
        if place:
            self.pose(f, *place)
        else:
            self.place_panel(f, ev)
        kscreen(f"output.{slot.output}.enable", f"output.{slot.output}.scale.{s:g}",
                f"output.{slot.output}.position.{slot.pos[0]},{slot.pos[1]}")
        self.rescaled(slot, s)
        self.command(cmd="place", id=wid, output=slot.output, x=slot.pos[0] + m / s, y=slot.pos[1] + m / s,
                     w=fr["w"], h=fr["h"], onAllDesktops=True)
        return f

    def pose(self, f, c, xa, ya, za):
        rows = [f"{xa[k]:.5f} {ya[k]:.5f} {za[k]:.5f} {c[k]:.4f}" for k in range(3)]
        self.screens.ask(f"pose {f.slot.index} {' '.join(rows)}")

    def set_panel(self, f, crop, title=0):
        x, y, w, h = crop
        self.screens.ask(f"float {f.slot.index} {f.mpp:.7f} {x} {y} {w} {h} {title}")

    def place_panel(self, f, ev):
        """Put the panel where the window was on its screen, a little in front of it."""
        m = re.match(r"WL-(\d+)$", ev["output"])
        reply = self.screens.ask(f"get {int(m.group(1)) + 1}", quiet=True) if m else ""
        if not reply.startswith("ok"):
            return
        g = ft_layout.parse_get(reply)
        c, xa, ya, za = g["center"], g["x"], g["y"], g["z"]
        out, fr, s = ev["outputRect"], ev["frame"], f.scale
        # The window's centre relative to the screen's, in panel pixels, then metres.
        dx = ((fr["x"] - out["x"]) + fr["w"] / 2 - out["w"] / 2) * s * f.mpp
        dy = ((fr["y"] - out["y"]) + fr["h"] / 2 - out["h"] / 2) * s * f.mpp
        p = [c[k] + xa[k] * dx - ya[k] * dy + za[k] * PULL_OUT for k in range(3)]
        self.pose(f, p, xa, ya, za)

    def follow(self, f, ev):
        """The window moved or resized on its output: crop the panel to it, and keep the
        output its size plus the margin. Full screen: no margin, and the output keeps the
        window's size from before, so the window fills its own panel."""
        slot, s = f.slot, f.scale
        fr, cl, out = ev["frame"], ev["client"], ev["outputRect"]
        f.frame, f.client = fr, cl
        # KWin sizes a window to its output before it reports it full screen; with a margin, a
        # window that fills its output is going full screen. (Not just after it left full
        # screen: then it fills the output until the output grows back.)
        fills = self.margin > 0 and (fr["x"], fr["y"], fr["w"], fr["h"]) == (out["x"], out["y"], out["w"], out["h"])
        full = bool(ev.get("fullScreen")) or (fills and time.monotonic() > f.unfull_until)
        f.full = full
        w, h = round(fr["w"] * s), round(fr["h"] * s)
        if not full and not (f.normal and abs(f.normal[0] - w) <= math.ceil(s) and abs(f.normal[1] - h) <= math.ceil(s)):
            # (Within a logical pixel it's the same size: a size asked for in pixels comes out
            # rounded to whole logical pixels. Keeping it stops scale changes from creeping.)
            f.normal = (w, h)
        m = 0 if full else self.margin
        if f.resizing:
            # KWin ends a resize by the window's edge whenever an output changes: the output
            # follows when it's done (the margin is room to grow until then).
            x, y = round((fr["x"] - out["x"]) * s), round((fr["y"] - out["y"]) * s)
            self.set_panel(f, (x, y, w, h), title=round((cl["y"] - fr["y"]) * s))
            return
        self.set_size(slot, f.normal if full and f.normal else (w + 2 * m, h + 2 * m))
        if not full:
            x0, y0 = slot.pos[0] + m / s, slot.pos[1] + m / s
            if abs(fr["x"] - x0) > 0.5 or abs(fr["y"] - y0) > 0.5:
                self.command(cmd="geometry", id=f.id, x=x0, y=y0, w=fr["w"], h=fr["h"])
                return  # the next geometry event crops the panel
        x, y = round((fr["x"] - out["x"]) * s), round((fr["y"] - out["y"]) * s)
        self.set_panel(f, (x, y, w, h), title=0 if full else round((cl["y"] - fr["y"]) * s))

    def rescale(self, f, steps):
        """Meta+scroll: the window's content bigger or smaller, at the same size in pixels, so its
        panel stays the same size (KWin's output scale, in steps of 10%)."""
        if not f.frame or f.full or steps == 0:
            return
        self.set_scale(f, round(f.scale * 1.1 ** steps * 20) / 20)

    def set_scale(self, f, s):
        """The window's scale (KWin's output scale), at the same size in pixels."""
        s = min(3.0, max(0.5, s))
        if not f.frame or f.full or s == f.scale or self.floats.get(f.id) is not f:
            return
        w, h = f.normal or (round(f.frame["w"] * f.scale), round(f.frame["h"] * f.scale))
        slot, m = f.slot, self.margin
        log(f"{f.id[:9]} scale {f.scale:g} -> {s:g}")
        f.scale = s
        # The output's size in pixels must suit the new scale before KWin draws at it (see whole).
        k = whole(slot.kscale, s)
        if slot.size[0] % k or slot.size[1] % k:
            slot.want = None
            self.set_size(slot, slot.size, k)
            self.sized(slot, slot.size)
        kscreen(f"output.{slot.output}.scale.{s:g}")
        self.screens.ask(f"scale {slot.index} {s:g}")
        self.rescaled(slot, s)
        self.command(cmd="geometry", id=f.id, x=slot.pos[0] + m / s, y=slot.pos[1] + m / s, w=w / s, h=h / s)

    def rescaled(self, slot, s):
        """KWin has the spare at scale s now: ask for its size again in the new scale's terms,
        or the next configure (any size, or KWin's own) would make it the old size times s."""
        if s == slot.kscale:
            return
        slot.kscale = s
        want, slot.want = slot.size, None
        self.set_size(slot, want)

    def dock(self, f, frame=None, output=None, maximized=False):
        """Back where it came from (or onto screen 1 if we don't know), or onto output at frame."""
        saved = f.saved or {"output": "WL-0", "frame": dict(f.frame or {"x": 100, "y": 100, "w": 800, "h": 600}),
                            "onAllDesktops": False}
        fr = frame or saved["frame"]
        if not output:
            output = saved["output"]
            m = re.match(r"WL-(\d+)$", output)
            hidden = self.concealed()
            if m and int(m.group(1)) + 1 in hidden:
                # It came from a screen that's hidden now: onto the first one that shows.
                shown = [i for i in range(self.screens_n) if i + 1 not in hidden]
                if shown:
                    x0, y0, ow, oh = output_rects().get(f"WL-{shown[0]}", (0, 0, 0, 0))
                    output = f"WL-{shown[0]}"
                    w, h = min(fr["w"], ow or fr["w"]), min(fr["h"], oh or fr["h"])
                    fr = {"x": x0 + max(0, (ow - w) / 2), "y": y0 + max(0, (oh - h) / 2), "w": w, "h": h}
        log(f"{f.id[:9]} back to {output}")
        self.command(cmd="place", id=f.id, output=output, x=fr["x"], y=fr["y"], w=fr["w"], h=fr["h"],
                     onAllDesktops=bool(saved.get("onAllDesktops")), maximized=maximized)

    def remember(self, f):
        """Keep where an app's window floated (before its panel goes)."""
        if not f.app or not f.frame:
            return
        g, ref = self.panel_get(f.slot.index), self.reference()
        if not g or not ref:
            return
        pixels = list(f.normal or (round(f.frame["w"] * f.scale), round(f.frame["h"] * f.scale)))
        self.places[f.app] = {"rel": to_local(ref, g), "pixels": pixels, "scale": f.scale,
                              "mpp": round(f.mpp, 8)}
        try:
            save_places(self.places)
        except OSError as e:
            log(f"couldn't save {PLACES_PATH}: {e}")

    def release(self, f):
        """Its window left: hide the panel and turn the spare off."""
        self.remember(f)
        slot = f.slot
        if slot.window is f:
            slot.window = None
            slot.want = None
        for sub_id in list(f.subs):
            self.drop_sub(sub_id)
        self.screens.ask(f"unfloat {slot.index}", quiet=True)
        kscreen(f"output.{slot.output}.disable")

    # ------------------------------------------------------------ launching floating

    def panel_get(self, index):
        reply = self.screens.ask(f"get {index}", quiet=True)
        return ft_layout.parse_get(reply) if reply.startswith("ok") else None

    def primary(self):
        """The primary screen (0-based): the one with the taskbar."""
        try:
            return min(ft_layout.primary_screen(ft_layout.load_layout()), self.screens_n - 1)
        except (OSError, ValueError, KeyError):
            return 0

    def reference(self):
        """The primary screen's panel: remembered places are relative to it, so they move with
        the screens' layout."""
        return self.panel_get(self.primary() + 1)

    def in_front(self):
        """A place in front of you, facing you, a little nearer than the primary screen (so text
        looks as big as on the screens, at their density)."""
        reply = self.screens.ask("head", quiet=True)
        ref = self.reference()
        if not reply.startswith("ok"):
            return None
        h = reply.split()
        eye, heading = [float(v) for v in h[1:4]], float(h[4])
        # (Within reach: you may have walked away from the screens since they were arranged.)
        d = min(IN_FRONT, max(0.8, math.dist(eye, ref["center"]) - PULL_OUT)) if ref else IN_FRONT
        fwd = ft_layout.turn_yaw((0.0, 0.0, -1.0), heading)
        c = [eye[k] + fwd[k] * d for k in range(3)]
        c[1] -= 0.1 * d  # a little below eye level, like a screen
        return c, ft_layout.turn_yaw((1.0, 0.0, 0.0), heading), (0.0, 1.0, 0.0), ft_layout.turn_yaw((0.0, 0.0, 1.0), heading)

    def launch(self, app=None, argv=None):
        """Start an app (a desktop file name) or a command; its first window will float.
        Returns (reply, pid or None)."""
        if app:
            app = app.removesuffix(".desktop")
            try:
                info = Gio.DesktopAppInfo.new(app + ".desktop")
            except TypeError:  # PyGObject raises for the NULL a missing desktop file returns
                info = None
            if info is None:
                return f"error no app {app}", None
            pids = []
            try:
                info.launch_uris_as_manager([], Gio.AppLaunchContext(), GLib.SpawnFlags.SEARCH_PATH,
                                            None, None, lambda _info, pid, *_: pids.append(pid), None)
            except GLib.Error as e:
                return f"error {app}: {e.message}", None
            self.launches.append(Launch(app, pids[0] if pids else None))
            log(f"launched {app} (pid {pids[0] if pids else 'by D-Bus'})")
            return "ok", (pids[0] if pids else None)
        else:
            try:
                proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                        stderr=subprocess.DEVNULL, start_new_session=True)
            except OSError as e:
                return f"error {argv[0]}: {e}", None
            GLib.child_watch_add(GLib.PRIORITY_DEFAULT, proc.pid, lambda *_: None)  # reap it
            self.launches.append(Launch("", proc.pid))
            log(f"started {argv[0]} (pid {proc.pid})")
            return "ok", proc.pid

    def launched(self, ev):
        """Is this new window the one an app we started was to open? Takes it off the list."""
        now = time.monotonic()
        self.launches = [la for la in self.launches if la.until > now]
        if not ev.get("normal") or ev.get("popup") or ev.get("transient"):
            return None
        chain = pid_chain(int(ev.get("pid") or 0))
        app = ev.get("app", "")
        for la in self.launches:
            if any(p in chain for p in la.pids) or (la.app and app and la.app == app):
                if not la.entries or len(la.entries) <= 1:
                    self.launches.remove(la)
                return la
        return None

    def concealed(self):
        """The screens (1-based) hidden on their own (ft-layout hide N)."""
        reply = self.screens.ask("concealed", quiet=True)
        return {int(w) for w in reply.split()[1:] if w.isdigit()} if reply.startswith("ok") else set()

    def on_hidden_screen(self, ev):
        """A new top-level window on a screen that's hidden on its own."""
        m = re.match(r"WL-(\d+)$", ev.get("output", ""))
        if not m or int(m.group(1)) >= self.screens_n:
            return False
        if not ev.get("normal") or ev.get("popup") or ev.get("transient") or ev.get("cls") == "ksplashqml":
            return False
        return int(m.group(1)) + 1 in self.concealed()

    def float_launched(self, ev, launch):
        """Float a launched app's window (or one that opened on a hidden screen) where that app
        last floated (its size too), or in front of you, at the primary screen's density."""
        app = ev.get("app") or (launch.app if launch else "")
        known = self.places.get(app) if app else None
        ref = self.reference()
        place = to_world(ref, known["rel"]) if known and ref and len(known.get("rel", [])) == 12 else self.in_front()
        mpp = (known or {}).get("mpp") or self.screen_mpp(f"WL-{self.primary()}")
        scale = None
        if known and known.get("pixels"):
            scale = known.get("scale") or output_scales().get(ev["output"], 1.0)
            ev = dict(ev, frame=dict(ev["frame"], w=known["pixels"][0] / scale, h=known["pixels"][1] / scale))
        self.float_window(dict(ev, app=app), place=place, mpp=mpp, scale=scale)

    # ------------------------------------------------------------ profiles (docs/profiles.md)

    def capture(self, sender):
        """"windows": have the script report every window as it is now, and answer when the
        report is in (or after 3 seconds with what we know)."""
        if not sender:
            return "error windows needs a reply address"
        token = self.next_token
        self.next_token += 1
        self.captures[token] = sender
        self.command(cmd="report-all", token=token)
        GLib.timeout_add(3000, lambda: self.captured(token) and False)
        return None  # answered by captured

    def captured(self, token):
        sender = self.captures.pop(token, None)
        if sender and self.sock:
            try:
                self.sock.sendto(("ok " + json.dumps(self.window_entries())).encode(), sender)
            except OSError as e:
                log(f"windows: {e}")

    def window_entries(self):
        """Every app window and where it is, as a profile keeps it."""
        ref = self.reference()
        rects = None
        out = []
        for wid, ev in self.windows.items():
            if not recordable(ev):
                continue
            entry = {"app": ev["app"]} if ev.get("app") else {"cmd": cmdline(ev.get("pid")), "class": ev.get("cls", "")}
            if not entry.get("app") and not entry.get("cmd"):
                continue
            f = self.floats.get(wid)
            if f:
                g = self.panel_get(f.slot.index)
                if not g or not ref or not f.frame:
                    continue
                pixels = list(f.normal or (round(f.frame["w"] * f.scale), round(f.frame["h"] * f.scale)))
                entry["float"] = {"rel": to_local(ref, g), "pixels": pixels, "scale": f.scale, "mpp": round(f.mpp, 8)}
            else:
                m = re.match(r"WL-(\d+)$", ev.get("output", ""))
                if not m or int(m.group(1)) >= self.screens_n:
                    continue
                fr, o = ev["frame"], ev.get("outputRect")
                if not o:
                    rects = rects if rects is not None else output_rects()
                    x0, y0, _, _ = rects.get(ev["output"], (0, 0, 0, 0))
                    o = {"x": x0, "y": y0}
                entry["screen"] = int(m.group(1)) + 1
                entry["rect"] = [round(fr["x"] - o["x"]), round(fr["y"] - o["y"]), round(fr["w"]), round(fr["h"])]
                if ev.get("maximized"):
                    entry["maximized"] = True
            out.append(entry)
        return out

    def window_key(self, ev):
        return ev.get("app") or json.dumps(cmdline(ev.get("pid")))

    def open_profile(self, name):
        """Open a profile's apps (additive: nothing closes)."""
        try:
            profile = ft_layout.load_layout().get("profiles", {}).get(name)
        except (OSError, ValueError):
            profile = None
        if profile is None:
            return f"error no profile {name!r}"
        groups = {}
        for e in profile.get("windows", []):
            key = e.get("app") or json.dumps(e.get("cmd") or [])
            if key != "[]":
                groups.setdefault(key, []).append(e)
        claimed = set()
        for key, entries in groups.items():
            have = [ev for wid, ev in self.windows.items()
                    if wid not in claimed and recordable(ev) and self.window_key(ev) == key]
            for e, ev in zip(entries, have):
                claimed.add(ev["id"])
                self.place_entry(ev, e)
            rest = entries[len(have):]
            if rest:
                self.launch_entries(rest)
        log(f"profile {name!r}: {sum(len(v) for v in groups.values())} windows, {len(claimed)} already open")
        return "ok"

    def launch_entries(self, entries):
        """Launch an app for a profile's entries that have no window yet."""
        e = entries[0]
        if e.get("app"):
            reply, pid = self.launch(app=e["app"])
            app = e["app"]
        else:
            reply, pid = self.launch(argv=e["cmd"])
            app = ""
        if not reply.startswith("ok"):
            log(f"profile: {reply}")
            return
        la = self.launches[-1]  # the one launch() just added
        la.entries, la.argv = list(entries), (None if app else e["cmd"])

    def relaunch(self, la):
        """3 seconds after a profile's app showed its first window: start it again for each
        entry still waiting (an app that restores its own windows has shown them by now)."""
        if la.relaunched or not la.entries or la not in self.launches:
            return
        la.relaunched = True
        la.until = time.monotonic() + LAUNCH_SECONDS
        for _ in la.entries:
            if la.app:
                reply, pid = self.launch(app=la.app)
            else:
                reply, pid = self.launch(argv=la.argv)
            if reply.startswith("ok"):
                self.launches.pop()  # launch() added one of its own; this one waits for them all
                if pid:
                    la.pids.append(pid)

    def place_entry(self, ev, e):
        """Move a window to a profile entry's place: on a screen, or floating."""
        f = self.floats.get(ev["id"])
        if "float" in e:
            fl, ref = e["float"], self.reference()
            if not ref or len(fl.get("rel", [])) != 12:
                return
            place = to_world(ref, fl["rel"])
            pixels, scale = fl.get("pixels"), fl.get("scale")
            if f:
                self.pose(f, *place)
                if scale:
                    self.set_scale(f, scale)
                if pixels and f.frame:
                    m = self.margin
                    self.command(cmd="geometry", id=f.id, x=f.slot.pos[0] + m / f.scale,
                                 y=f.slot.pos[1] + m / f.scale, w=pixels[0] / f.scale, h=pixels[1] / f.scale)
                return
            if pixels:
                scale = scale or output_scales().get(ev["output"], 1.0)
                ev = dict(ev, frame=dict(ev["frame"], w=pixels[0] / scale, h=pixels[1] / scale))
            else:
                scale = None
            self.float_window(ev, place=place, mpp=fl.get("mpp"), scale=scale)
            return
        n = int(e.get("screen", 1)) - 1
        if not 0 <= n < self.screens_n:
            n = 0
        name = f"WL-{n}"
        x0, y0, ow, oh = output_rects().get(name, (0, 0, 0, 0))
        rx, ry, rw, rh = (e.get("rect") or [100, 100, ev["frame"]["w"], ev["frame"]["h"]])[:4]
        if ow and oh:  # keep it on the screen if the screen got smaller
            rw, rh = min(rw, ow), min(rh, oh)
            rx, ry = max(0, min(rx, ow - rw)), max(0, min(ry, oh - rh))
        frame = {"x": x0 + rx, "y": y0 + ry, "w": rw, "h": rh}
        if f:
            self.dock(f, frame=frame, output=name, maximized=bool(e.get("maximized")))
        else:
            self.command(cmd="place", id=ev["id"], output=name, x=frame["x"], y=frame["y"], w=rw, h=rh,
                         maximized=bool(e.get("maximized")))

    # ------------------------------------------------------------ popups and dialogs

    def sub(self, ev):
        parent = self.floats.get(ev.get("parent", ""))
        if parent is None:
            # A popup of a popup: its top-level parent is the floating window.
            known = self.sub_numbers.get(ev.get("parent", ""))
            parent = self.floats.get(known[0]) if known else None
        if parent is None:
            return
        wid = ev["id"]
        if wid not in self.sub_numbers:
            self.sub_numbers[wid] = (parent.id, self.next_sub)
            parent.subs[wid] = self.next_sub
            self.next_sub += 1
        n = self.sub_numbers[wid][1]
        fr, out, s = ev["frame"], ev["outputRect"], parent.scale
        x, y = round((fr["x"] - out["x"]) * s), round((fr["y"] - out["y"]) * s)
        self.screens.ask(f"sub {parent.slot.index} {n} {x} {y} {round(fr['w'] * s)} {round(fr['h'] * s)}", quiet=True)

    def drop_sub(self, wid):
        known = self.sub_numbers.pop(wid, None)
        if not known:
            return
        parent = self.floats.get(known[0])
        if parent:
            parent.subs.pop(wid, None)
            self.screens.ask(f"sub {parent.slot.index} {known[1]} off", quiet=True)

    # ------------------------------------------------------------ requests on @frametop_float

    def by_panel(self, index):
        for s in self.slots:
            if s.index == index:
                return s.window
        return None

    def request(self, text, sender=None):
        words = text.split()
        if not words:
            return "error empty"
        cmd, rest = words[0], words[1:]
        if cmd == "launch" and len(rest) == 1:
            return self.launch(app=rest[0])[0]
        if cmd == "run" and rest:
            try:
                argv = json.loads(text.split(None, 1)[1])
            except ValueError:
                return "error run takes a JSON list"
            if not isinstance(argv, list) or not argv or not all(isinstance(a, str) for a in argv):
                return "error run takes a JSON list"
            return self.launch(argv=argv)[0]
        if cmd == "windows":
            return self.capture(sender)
        if cmd == "profile" and rest:
            return self.open_profile(text.split(None, 1)[1])
        if cmd == "list":
            return "ok " + " ".join(f"{s.output}:{s.window.id if s.window else '-'}" for s in self.slots)
        if cmd == "quit":
            GLib.idle_add(self.loop.quit)
            return "ok"
        if cmd in ("float", "dock") and (not rest or rest[0] == "active"):
            self.command(cmd="request-active")
            return "ok"
        if cmd == "float" and rest == ["pointer"]:
            self.command(cmd="request-pointer")
            return "ok"
        if cmd == "dock" and rest == ["all"]:
            for f in list(self.floats.values()):
                self.dock(f)
            return "ok"
        if cmd in ("dock", "close", "resize", "scale") and rest and rest[0].isdigit():
            f = self.by_panel(int(rest[0]))
            if not f:
                return f"error no floating window on screen {rest[0]}"
            if cmd == "dock":
                self.dock(f)
            elif cmd == "close":
                self.command(cmd="close", id=f.id)
            elif cmd == "scale" and len(rest) == 2:
                self.rescale(f, int(rest[1]))
            elif cmd == "resize" and len(rest) == 3 and f.frame:
                w, h = max(320, int(rest[1])), max(200, int(rest[2]))
                self.command(cmd="geometry", id=f.id, x=f.frame["x"], y=f.frame["y"], w=w / f.scale, h=h / f.scale)
            return "ok"
        if cmd in ("float", "dock", "close") and rest:
            ev = self.windows.get(rest[0])
            if not ev:
                return f"error no window {rest[0]}"
            if cmd == "float":
                # The script's word for where it is now: a window on a screen isn't followed here.
                self.command(cmd="request-float", id=rest[0])
            elif cmd == "dock" and rest[0] in self.floats:
                self.dock(self.floats[rest[0]])
            elif cmd == "close":
                self.command(cmd="close", id=rest[0])
            return "ok"
        return "error unknown command"

    def notify(self, text):
        log(text)
        try:
            n = dbus.Interface(dbus.SessionBus().get_object("org.freedesktop.Notifications",
                                                            "/org/freedesktop/Notifications"),
                               "org.freedesktop.Notifications")
            n.Notify("Frametop", 0, "window-new", "Floating windows", text, [], {}, 5000)
        except dbus.DBusException as e:
            log(f"notification: {e.get_dbus_message()}")


class Service(dbus.service.Object):
    def __init__(self, bus, daemon):
        super().__init__(dbus.service.BusName(SERVICE, bus), PATH)
        self.daemon = daemon

    @dbus.service.method(IFACE, in_signature="s", out_signature="")
    def Event(self, text):
        try:
            self.daemon.on_event(json.loads(text))
        except (ValueError, KeyError, TypeError) as e:
            log(f"bad event {text[:200]}: {e!r}")

    @dbus.service.method(IFACE, in_signature="", out_signature="s", async_callbacks=("reply", "error"))
    def NextCommand(self, reply, error):
        self.daemon.wait(reply)


def watch_apps(daemon):
    """Keep Launch as Standalone's desktop file copies up to date (float/ft_apps.py), in a
    session that reads them (the session script puts them in XDG_DATA_DIRS)."""
    ours = os.path.realpath(ft_apps.OUT_ROOT)
    if ours not in (os.path.realpath(d) for d in os.environ.get("XDG_DATA_DIRS", "").split(":") if d):
        return
    later = []

    def write():
        later.clear()
        try:
            log(f"Launch as Standalone: {ft_apps.write_all()} apps")
        except OSError as e:
            log(f"Launch as Standalone: {e}")
        return False

    def changed(*_):
        if not later:  # wait for an install or update to settle
            later.append(GLib.timeout_add_seconds(3, write))
    write()
    daemon.app_monitors = []
    for d in ft_apps.app_dirs():
        if os.path.isdir(d):
            m = Gio.File.new_for_path(d).monitor_directory(Gio.FileMonitorFlags.NONE, None)
            m.connect("changed", changed)
            daemon.app_monitors.append(m)


def main():
    conf = ft_layout.read_conf()
    p = argparse.ArgumentParser(description="Floating windows for the Frametop desktop")
    p.add_argument("--screens", type=int, default=int(os.environ.get("FT_SCREEN_COUNT") or 0))
    p.add_argument("--slots", type=int, default=int(conf.get("FLOAT_SLOTS") or 8))
    p.add_argument("--margin", type=int, default=int(conf.get("FLOAT_MARGIN") or 300))
    p.add_argument("--control", default="ft_screens")
    p.add_argument("--socket", default="frametop_float")
    args = p.parse_args()
    if args.screens <= 0:
        args.screens = ft_layout.screen_count()
    args.slots = max(0, min(16, args.slots))
    args.margin = max(0, min(1000, args.margin))

    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SessionBus()
    daemon = Daemon(args)
    service = Service(bus, daemon)  # noqa: F841 (keeps the name)
    daemon.loop = GLib.MainLoop()

    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind("\0" + args.socket)
    sock.setblocking(False)
    daemon.sock = sock

    def readable(*_):
        while True:
            try:
                data, sender = sock.recvfrom(4096)
            except BlockingIOError:
                return True
            reply = daemon.request(data.decode(errors="replace").strip(), sender)
            if sender and reply is not None:
                try:
                    sock.sendto(reply.encode(), sender)
                except OSError:
                    pass
    GLib.io_add_watch(sock.fileno(), GLib.IO_IN, readable)

    log(f"{args.screens} screens, {args.slots} floating slots (WL-{args.screens} and up), margin {args.margin} px")
    daemon.load_script(bus)
    watch_apps(daemon)
    daemon.loop.run()


if __name__ == "__main__":
    main()
