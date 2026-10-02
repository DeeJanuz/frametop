#!/usr/bin/env python3
"""Input relay for the Steam Frame: stable virtual devices, the 3D pointer, device rules.

SteamVR opens /dev/input/event* only when it starts and never hotplugs, so a
Bluetooth mouse that sleeps and reconnects (new event nodes) stops working
until SteamVR restarts. This relay creates a virtual mouse and a virtual
keyboard through /dev/uinput once, before SteamVR starts, and feeds them from
the physical devices as they come and go.

Every USB or Bluetooth mouse and keyboard is a candidate. A physical device is
identified by its Bluetooth address (EVIOCGUNIQ) or USB bus:vendor:product:name,
so all its event nodes share one role (the Swiftpoint Z3 has a mouse node and a
keyboard node for its extra buttons). Roles, from ~/.config/frametop-input.json
(written by the Frametop Input Settings app):
  pointer      grabbed; drives the universal 3D mouse (default for devices with a mouse node)
  passthrough  keys go to the desktop; grabbed only while typing goes there, otherwise only observed,
               e.g. for the Meta dashboard shortcut (default for keyboards; the shortcut is off
               unless META_DASHBOARD=1 is in ~/.config/frametop.conf)
  ignore       not grabbed, only observed for identification in the settings app
Buttons and keys of pointer devices go through a per-device map to actions
(left, right, middle, back, scroll_up, scroll_down, dashboard, recenter,
pointer_toggle, follow_toggle = head follow on or off, gaze_toggle = gaze mode on or off
(the pointer goes where you look; see pointer/helper/ft-pointer.cpp), gaze_precision = while
held, the pointer stops where you look and the mouse steers it, and the release clicks there,
gaze_drag = the same, but pressed at once, so it drags ("precision|gazedrag mouse|keyboard 1|0"
to the helper), gaze_left and gaze_right = keyboard clicks at the gaze: a tap clicks where you
look; held, the pointer stops there and your head steers it (it stays put in your view), and
the release clicks; held still for half a second, it's a real press that your head drags
("gazekey left|right 1|0" to the helper; by default Meta+J and Meta+K, DEFAULT_KEY_BINDINGS),
gaze_quickcal = the gaze service's one-dot check ("quickcal" to @ft_gazed), sens_up, sens_down,
layout_reset = put the desktop screens back in their saved layout, screens_toggle = hide or show the desktop screens,
keyboard_toggle = open or close Frametop's keyboard, float_toggle = float the desktop window under the
pointer (else the active one) in VR, or put it back if it floats, dock_all = put every floating
window back (both to ft-floatd, @frametop_float), profile:NAME = switch to that profile (ft-layout
use NAME: its screens and apps; docs/profiles.md), key = pass through as a key, none).

Frame controller buttons can be mapped too ("controller_buttons": {"right/a": action} in the
rules file; any action but key and the gaze ones, GAZE_ACTIONS: gaze mode is a mouse feature,
docs/gaze-controllers.md). So can key combinations on any keyboard ("key_bindings":
{"29+56+34": action}, evdev codes joined by "+", modifiers first and left-hand codes for
either side, here Ctrl+Alt+G): the combination does the action, and its last key isn't typed.
A rules file without "key_bindings" gets DEFAULT_KEY_BINDINGS (Meta+J: gaze_left, Meta+K:
gaze_right, Meta+Shift+F: float_toggle); one with its own, even an empty one, doesn't. The float
actions work without pointer mode too. A combination with Meta also sends the desktop an F24 press
and Meta's release right away: so letting go of Meta doesn't open Plasma's launcher, and a gaze
click isn't Meta+click (KWin's window move and resize). Another key while Meta is still held gives
the desktop Meta back. The controllers aren't input devices here, only SteamVR sees
them, so the pointer helper reads them with SteamVR input and sends "vrbtn <button> 1|0".
It only takes the buttons the relay tells it to ("vrbind <button>..." to @ft_pointer_helper,
sent on start, reload, and when the helper says "vrhello"), and only while no game runs,
unless "controller_in_games" is true in the rules file (then a mapped button no longer
reaches games; see pointer/helper/vrbuttons.h).

In gaze mode outside games, the helper keeps the pointer ("gazeawake 1", repeated every 5
seconds; "gazeawake 0" or silence ends it): the pointer isn't released when the mouse is idle.
Typing on a keyboard sends the helper "typing" (at most 4 times a second): it takes no hand
pinches right after a key, since typing touches thumb to index like a pinch.

Keys also go to ft-screens (@ft_screens, the Frametop desktop's compositor), which
types them into the desktop screen that has focus: from pass-through keyboards, and
keys a pointer device passes through. Typing goes to the panel clicked last, and
ft-screens says which ("keyboard desktop|steam" on the control socket, every second).
While it's the desktop, pass-through keyboards are grabbed, so gamescope, which reads
every keyboard itself, doesn't type them into its focused app too. Without word from
ft-screens for 3 seconds they're released. With SHARE_KEYS=1 in ~/.config/frametop.conf,
a grabbed keyboard's keys also go out as "key <code> <value> <device name>" datagrams on
@frametop_keys, for programs that watch every keyboard for a hotkey and lose it to the grab.
It's off by default: any local process that binds that name first gets every key typed
into the desktop.

Frametop's keyboard (ft-screens' key panel): the desktop's input method (input/ft-textinput)
says "textfield 1|0" when a text field on the desktop gains or loses keyboard focus, and
the relay tells ft-screens to open ("vrkeyboard show") or close ("vrkeyboard hide") the
keyboard, depending on "vr_keyboard" in the rules file: "always", "no_keyboard" (the
default: only while no pass-through keyboard is connected; a program's uinput keyboard
doesn't count), "button" (only the keyboard_toggle action opens it), or "never"
(keyboard_toggle does nothing either). With "vr_keyboard_persist" (the default), it stays
open when the text field loses focus, until its Close key, keyboard_toggle, or a layout reset
(ft-layout apply) closes it.

Volume keys, from every device that has them (the headset's own buttons included),
are handled here: wpctl steps the default output. Nothing else may see a volume key,
because gamescope aborts on one when no window has keyboard focus, which ends the
whole VR session. Devices with a keymap (the headset's gpio-keys, USB and Bluetooth
keyboards) get their volume entries remapped to unused stand-in codes, so their
other keys keep working for SteamVR; a device without a keymap that has only volume
keys (the headset's pmic_resin) is grabbed. The keymaps go back when the relay exits.

Pointer mode (POINTER=1 in ~/.config/frametop.conf) sends pointer devices to
the ft-pointer helper (pointer/helper), which drives the ft_pointer
SteamVR driver. With POINTER=0, pointer devices go to the virtual mouse and
keyboard instead.

Control socket (abstract datagram @frametop_relay, JSON replies to the sender):
  devices           list event nodes with id, name, kinds, role, grabbed
  watch <seconds>   stream input events from every candidate node (identification)
  reload            re-read both config files, re-apply roles, tell the helper
  vrcapture <s>     take every controller button for s seconds (0: stop), so the settings
                    app can capture one; watchers see them as events with id frame_controller
  vrbtn, vrhello, gazeawake   from the pointer helper (above)
  textfield 1|0     from the desktop's input method (above)

Runs on the Frame host as a user service (frametop-input-relay.service). The
virtual devices are parked in systemd's file descriptor store, so a relay
restart gets the same devices back and SteamVR never loses them. The service is
Type=notify: READY=1 goes out only after the devices exist, so SteamVR (ordered
after it) always finds them. Dependency-free: Python standard library plus the
kernel's evdev and uinput interfaces.

  input-relay.py            the service
  input-relay.py --no-grab  never grab, for testing next to a running SteamVR
"""
import array
import atexit
import errno
import fcntl
import json
import math
import os
import select
import signal
import socket
import struct
import subprocess
import sys
import time

# Linux input constants (include/uapi/linux/input-event-codes.h, input.h, uinput.h).
EV_SYN, EV_KEY, EV_REL, EV_MSC = 0x00, 0x01, 0x02, 0x04
SYN_REPORT = 0
BTN_MISC, KEY_MAX = 0x100, 0x2FF
KEY_A = 30
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL, REL_MAX = 0x00, 0x01, 0x06, 0x08, 0x0F
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA = 0x110, 0x111, 0x112, 0x113, 0x114
KEY_LEFTMETA, KEY_RIGHTMETA = 125, 126
KEY_VOLUMEDOWN, KEY_VOLUMEUP = 114, 115
# Volume keys are remapped to KEY_MACRO29 and KEY_MACRO30: above 255, so X11 can't
# carry them, and bound to nothing in the default keymap.
VOLUME_STANDIN = {KEY_VOLUMEUP: 0x2AC, KEY_VOLUMEDOWN: 0x2AD}
VOLUME_ORIGINAL = {v: k for k, v in VOLUME_STANDIN.items()}
VOLUME_CODES = set(VOLUME_STANDIN) | set(VOLUME_ORIGINAL)
BUS_USB, BUS_BLUETOOTH, BUS_VIRTUAL = 0x03, 0x05, 0x06

# struct input_event on 64-bit: struct timeval (2 x long), u16 type, u16 code, s32 value.
EVENT = struct.Struct("llHHi")


def _ioc(direction, nr, size, kind):
    return (direction << 30) | (size << 16) | (ord(kind) << 8) | nr


def _iow(kind, nr, size):
    return _ioc(1, nr, size, kind)


def _ior(kind, nr, size):
    return _ioc(2, nr, size, kind)


UI_DEV_CREATE = _ioc(0, 1, 0, "U")
UI_DEV_DESTROY = _ioc(0, 2, 0, "U")
UI_DEV_SETUP = _iow("U", 3, 92)  # struct uinput_setup: input_id (4 x u16), name[80], u32
UI_SET_EVBIT = _iow("U", 100, 4)
UI_SET_KEYBIT = _iow("U", 101, 4)
UI_SET_RELBIT = _iow("U", 102, 4)
EVIOCGRAB = _iow("E", 0x90, 4)
EVIOCGKEY = _ior("E", 0x18, (KEY_MAX + 8) // 8)
EVIOCGID = _ior("E", 0x02, 8)
KEYMAP_ENTRY = struct.Struct("BBHI32s")  # struct input_keymap_entry: flags, len, index, keycode, scancode
INPUT_KEYMAP_BY_INDEX = 1
EVIOCGKEYCODE_V2 = _ior("E", 0x04, KEYMAP_ENTRY.size)
EVIOCSKEYCODE_V2 = _iow("E", 0x04, KEYMAP_ENTRY.size)
EV_NAMES = {EV_KEY: "key", EV_REL: "rel"}


def eviocgbit(ev, length):
    return _ior("E", 0x20 + ev, length)


def eviocgname(length):
    return _ior("E", 0x06, length)


def eviocguniq(length):
    return _ior("E", 0x08, length)


VIRTUAL_PREFIX = "frametop virtual"
RULES_PATH = os.path.expanduser("~/.config/frametop-input.json")
ACTIONS = ("left", "right", "middle", "back", "scroll_up", "scroll_down", "dashboard", "recenter",
           "pointer_toggle", "follow_toggle", "gaze_toggle", "gaze_precision", "gaze_drag", "gaze_left", "gaze_right",
           "gaze_quickcal", "sens_up", "sens_down", "layout_reset", "screens_toggle", "keyboard_toggle", "float_toggle",
           "dock_all", "key", "none")
# Gaze mode is a mouse feature: these never come from a controller button (docs/gaze-controllers.md).
GAZE_ACTIONS = ("gaze_toggle", "gaze_precision", "gaze_drag", "gaze_left", "gaze_right", "gaze_quickcal")
# Key combinations a rules file without "key_bindings" gets: Meta+J and Meta+K click at the gaze
# (free on the Frametop desktop, and apps don't use Meta), Meta+Shift+F floats a window.
DEFAULT_KEY_BINDINGS = {"125+36": "gaze_left", "125+37": "gaze_right", "42+125+33": "float_toggle"}
KEY_F24 = 194  # sent to the desktop with a Meta combination (see the top)
# Key combinations ("key_bindings"): modifiers, each side's code folded into the left one's.
MODIFIERS = {29: 29, 97: 29, 42: 42, 54: 42, 56: 56, 100: 56, 125: 125, 126: 125}
VR_KEYBOARD_MODES = ("always", "no_keyboard", "button", "never")  # when Frametop's keyboard opens
HELPER = "\0ft_pointer_helper"
# Frame controller buttons the pointer helper can read (pointer/helper/vrbuttons.h).
VR_BUTTONS = ("left/view", "left/dpad_up", "left/dpad_down", "left/dpad_left", "left/dpad_right", "left/bumper",
              "left/trigger", "left/grip", "left/thumbstick", "right/menu", "right/a", "right/b", "right/x", "right/y",
              "right/bumper", "right/trigger", "right/grip", "right/thumbstick")
VR_DEVICE = "frame_controller"  # the id controller buttons have in watch events
SCREENS = "\0ft_screens"
GAZED = "\0ft_gazed"
FLOAT = "\0frametop_float"  # ft-floatd, floating windows in the Frametop desktop
# Actions for ft-floatd ("float_toggle", "dock_all"): they don't need pointer mode.
FLOAT_ACTIONS = {"float_toggle": b"float pointer", "dock_all": b"dock all"}
PROFILE = "profile:"  # "profile:NAME": switch to that profile (doesn't need pointer mode either)


def known_action(a):
    return a in ACTIONS or (isinstance(a, str) and a.startswith(PROFILE) and len(a) > len(PROFILE))


def needs_pointer(a):
    return a not in FLOAT_ACTIONS and not a.startswith(PROFILE)
KEYS = "\0frametop_keys"  # keys of keyboards grabbed for the desktop, for other readers
FT_LAYOUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "layout", "ft-layout")
DEFAULT_BUTTONS = {BTN_LEFT: "left", BTN_RIGHT: "right", BTN_MIDDLE: "middle",
                   BTN_SIDE: "back", BTN_EXTRA: "back"}


def log(*args):
    print(*args, flush=True)


def notify(state, fds=()):
    """sd_notify, with optional file descriptors for the fd store. No-op outside systemd."""
    addr = os.environ.get("NOTIFY_SOCKET")
    if not addr:
        return
    if addr.startswith("@"):
        addr = "\0" + addr[1:]
    # socket.send_fds() ignores its address argument (Python 3.12), so use sendmsg.
    ancillary = [(socket.SOL_SOCKET, socket.SCM_RIGHTS, array.array("i", fds))] if fds else []
    try:
        with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
            sock.sendmsg([state.encode()], ancillary, 0, addr)
    except OSError as e:
        log(f"sd_notify failed ({state.splitlines()[0]}): {e}")


def stored_fds():
    """File descriptors handed back by systemd's fd store, by name."""
    if os.environ.get("LISTEN_PID") != str(os.getpid()):
        return {}
    names = os.environ.get("LISTEN_FDNAMES", "").split(":")
    count = int(os.environ.get("LISTEN_FDS", "0"))
    return {names[i]: 3 + i for i in range(count) if i < len(names)}


class Virtual:
    """One uinput device, reused from systemd's fd store when possible."""

    def __init__(self, name, product, keys, rels, stored):
        self.dirty = False
        store_name = f"vdev{product}"
        if store_name in stored:
            self.fd = stored[store_name]
            os.set_blocking(self.fd, False)
            log(f"reusing {name} from the fd store")
            return
        self.fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_KEY)
        for code in keys:
            fcntl.ioctl(self.fd, UI_SET_KEYBIT, code)
        if rels:
            fcntl.ioctl(self.fd, UI_SET_EVBIT, EV_REL)
            for code in rels:
                fcntl.ioctl(self.fd, UI_SET_RELBIT, code)
        setup = struct.pack("HHHH80sI", BUS_VIRTUAL, 0x4D44, product, 1, name.encode(), 0)
        fcntl.ioctl(self.fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(self.fd, UI_DEV_CREATE)
        notify(f"FDSTORE=1\nFDNAME={store_name}", [self.fd])
        log(f"created {name}")

    def emit(self, etype, code, value):
        os.write(self.fd, EVENT.pack(0, 0, etype, code, value))
        self.dirty = True

    def sync(self):
        if self.dirty:
            os.write(self.fd, EVENT.pack(0, 0, EV_SYN, SYN_REPORT, 0))
            self.dirty = False


def bits(fd, ev, count):
    buf = bytearray((count + 7) // 8)
    try:
        fcntl.ioctl(fd, eviocgbit(ev, len(buf)), buf)
    except OSError:
        return set()
    return {i for i in range(count) if buf[i // 8] >> (i % 8) & 1}


def read_config(path=os.path.expanduser("~/.config/frametop.conf")):
    """KEY=VALUE lines, # comments allowed. Missing file means defaults."""
    conf = {}
    try:
        with open(path) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    key, value = line.split("=", 1)
                    conf[key.strip()] = value.strip()
    except OSError:
        pass
    return conf


def read_rules(path=RULES_PATH):
    """{"devices": {id: {"role", "name"}}, "buttons": {id: {"<code>": action}},
    "controller_buttons": {"<hand>/<button>": action}, "controller_in_games": bool,
    "key_bindings": {"<code>+<code>...": action},
    "vr_keyboard": one of VR_KEYBOARD_MODES, "vr_keyboard_persist": bool}."""
    try:
        with open(path) as f:
            rules = json.load(f)
    except (OSError, ValueError):
        rules = {}
    rules.setdefault("devices", {})
    rules.setdefault("buttons", {})
    rules.setdefault("controller_buttons", {})
    if not isinstance(rules.get("key_bindings"), dict):
        rules["key_bindings"] = dict(DEFAULT_KEY_BINDINGS)
    return rules


def remap_volume(fd, restore=False):
    """Point a device's volume keys at their stand-ins in its keymap, or back with restore.

    Returns how many keymap entries are volume keys or stand-ins, or None when the
    device has no keymap to change (uinput devices, some platform buttons).

    A swap that fails doesn't stop the others: every entry is still tried, then the
    first failure is raised. The entries that did swap stay swapped, for the caller
    to handle their stand-ins and restore them.
    """
    swap = VOLUME_ORIGINAL if restore else VOLUME_STANDIN
    found = 0
    failed = None  # the first swap that failed
    for index in range(8192):
        entry = bytearray(KEYMAP_ENTRY.pack(INPUT_KEYMAP_BY_INDEX, 0, index, 0, b""))
        try:
            fcntl.ioctl(fd, EVIOCGKEYCODE_V2, entry)
        except OSError:
            if not index:
                return None
            break  # past the last entry
        _, length, _, code, scancode = KEYMAP_ENTRY.unpack(entry)
        if code in VOLUME_CODES:
            found += 1
        if code in swap:
            try:
                fcntl.ioctl(fd, EVIOCSKEYCODE_V2,
                            KEYMAP_ENTRY.pack(INPUT_KEYMAP_BY_INDEX, length, index, swap[code], scancode))
            except OSError as e:
                if failed is None:
                    failed = e
    if failed is not None:
        raise failed
    return found


class Volume:
    """Volume keys: wpctl steps the default output, repeating while a key is held.

    The repeat is our own, since the headset's buttons have none; kernel autorepeat
    from keyboards is ignored so every device repeats the same way.
    """

    STEP = 5  # percent
    DELAY, RATE = 0.4, 0.1  # seconds before repeating, and between repeats

    def __init__(self):
        self.held = None  # (fd, code) of the key being held
        self.next_at = None

    def key(self, fd, code, value, now):
        if value == 1:
            self.held = (fd, code)
            self.step(code)
            self.next_at = now + self.DELAY
        elif value == 0 and self.held == (fd, code):
            self.release()

    def release(self):
        self.held = self.next_at = None

    def step(self, code):
        sign = "+" if VOLUME_ORIGINAL.get(code, code) == KEY_VOLUMEUP else "-"
        subprocess.Popen(["wpctl", "set-volume", "--limit", "1.0", "@DEFAULT_AUDIO_SINK@",
                          f"{self.STEP}%{sign}"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def tick(self, now):
        if self.next_at is not None and now >= self.next_at:
            self.step(self.held[1])
            self.next_at = now + self.RATE

    def timeout(self, now, default):
        return default if self.next_at is None else max(0.0, min(default, self.next_at - now))


class Pointer:
    """Drives the ft_pointer SteamVR driver from a mouse (pointer mode).

    The virtual controller connects when the mouse is used (taking the right
    hand role and recentering on the gaze) and disconnects after `idle` seconds
    without mouse activity, so the real controllers get their role back: the
    last used device wins.
    """

    DRIVER_BUTTONS = {"left": "trigger", "right": "b", "middle": "x", "back": "joystick"}
    SCROLL_PULSE = 0.08  # seconds of joystick deflection per wheel notch
    CLAIM_PULSE = 0.06  # seconds the claim button (switchlaserhand, no click) is held
    RESUME_PAUSE = 1.5  # mouse idle this long, then moving again, re-claims the laser
    WAKE_WINDOW = 1.0  # seconds in which WAKE_COUNTS of motion must add up

    def __init__(self, sensitivity, idle, wake_counts=40):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sensitivity = sensitivity  # degrees per mouse count
        self.idle = idle
        self.active = False
        self.last_used = 0.0
        self.dx = self.dy = 0
        self.scroll_until = None
        self.claim_at = None  # when to press the claim button
        self.claim_release = None
        self.system_at = None  # dashboard toggle: when to press the virtual system button
        self.system_release = None
        # Waking (or re-claiming after a pause) needs deliberate movement, so sensor
        # jitter from a mouse lying on a desk can't steal the laser from a controller.
        self.wake_counts = wake_counts
        self.pending = 0
        self.pending_since = 0.0
        self.gaze_awake_until = 0.0  # the helper's gaze mode keeps the pointer until then

    def send(self, command):
        try:
            self.sock.sendto(command.encode(), HELPER)
        except OSError:
            pass  # helper not running (SteamVR not running)

    def wake(self, now):
        if not self.active:
            self.send("show")
            self.send("recenter")
            self.active = True
            self.claim_at = now + 0.3  # let SteamVR bind the freshly connected device first
            log("pointer on")
        elif now - self.last_used > self.RESUME_PAUSE and self.claim_at is None:
            self.claim_at = now  # another device may have taken the laser meanwhile
        self.last_used = now

    def motion(self, code, value, now):
        dormant = not self.active or now - self.last_used > self.RESUME_PAUSE
        if dormant and code in (REL_X, REL_Y):
            if now - self.pending_since > self.WAKE_WINDOW:
                self.pending, self.pending_since = 0, now
            self.pending += abs(value)
            if self.pending < self.wake_counts:
                return  # not yet deliberate movement
            self.pending = 0
        self.wake(now)
        if code == REL_X:
            self.dx += value
        elif code == REL_Y:
            self.dy += value
        elif code == REL_WHEEL and value:
            self.send(f"scroll 0 {1 if value > 0 else -1}")
            self.scroll_until = now + self.SCROLL_PULSE

    def action(self, name, value, now, source="mouse"):
        """A mapped button: value 1 press, 0 release, 2 autorepeat (ignored). source: what
        pressed it (mouse, left, right for a controller, keyboard), for the gaze actions,
        which take the mouse or the keyboard only."""
        if value == 2:
            return
        if name in ("gaze_left", "gaze_right"):
            if value == 1:
                self.wake(now)
            self.flush()
            self.send(f"gazekey {name[5:]} {value}")
            return
        if name in ("gaze_precision", "gaze_drag"):
            if source not in ("mouse", "keyboard"):
                return  # gaze mode is a mouse feature (GAZE_ACTIONS)
            if value == 1:
                self.wake(now)
            self.flush()
            self.send(f"{'precision' if name == 'gaze_precision' else 'gazedrag'} {source} {value}")
            return
        driver = self.DRIVER_BUTTONS.get(name)
        if driver:
            self.wake(now)
            self.flush()
            self.send(f"btn {driver} {value}")
        elif value != 1:
            return  # the rest act on press
        elif name in ("scroll_up", "scroll_down"):
            self.wake(now)
            self.send(f"scroll 0 {1 if name == 'scroll_up' else -1}")
            self.scroll_until = now + self.SCROLL_PULSE
        elif name == "dashboard":
            self.dashboard(now)
        elif name == "recenter":
            self.wake(now)
            self.send("recenter")
        elif name == "pointer_toggle":
            if self.active:
                self.send("hide")
                self.active = False
                log("pointer off (toggle)")
            else:
                self.wake(now)
        elif name == "follow_toggle":
            self.send("follow toggle")  # until the next restart; the setting is POINTER_FOLLOW
            log("head follow toggled")
        elif name == "gaze_toggle":
            self.send("gaze toggle")  # until the next restart; the setting is POINTER_GAZE
            log("gaze mode toggled")
        elif name == "gaze_quickcal":
            try:
                self.sock.sendto(b"quickcal", GAZED)
            except OSError:
                pass  # the gaze service isn't running
        elif name == "screens_toggle":
            try:
                self.sock.sendto(b"toggle", SCREENS)
            except OSError:
                pass  # ft-screens not running
        elif name == "layout_reset":
            # Runs a few seconds and borrows the pointer; ft-layout refuses a second copy.
            subprocess.Popen([FT_LAYOUT, "apply"], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, start_new_session=True)
            log("layout reset")
        elif name in ("sens_up", "sens_down"):
            self.sensitivity *= 1.25 if name == "sens_up" else 0.8
            log(f"sensitivity {self.sensitivity:.4f} deg/count")

    def flush(self):
        if self.dx or self.dy:
            # Mouse right turns the ray right (negative yaw); mouse down tilts it down.
            self.send(f"move {-self.dx * self.sensitivity:.4f} {-self.dy * self.sensitivity:.4f}")
            self.dx = self.dy = 0

    def dashboard(self, now=None):
        """Toggle the SteamVR dashboard with the virtual controller's system button.

        SteamVR needs the button held for a frame or two (press and release in the
        same instant is ignored), and the virtual controller must be connected and
        bound, so wake it first when needed.
        """
        now = time.monotonic() if now is None else now
        woke = not self.active
        self.wake(now)
        self.system_at = now + (0.4 if woke else 0.0)

    def tick(self, now):
        if self.system_at is not None and now >= self.system_at:
            self.send("btn system 1")
            self.system_at = None
            self.system_release = now + 0.12
        elif self.system_release is not None and now >= self.system_release:
            self.send("btn system 0")
            self.system_release = None
        if self.claim_at is not None and now >= self.claim_at:
            self.send("btn a 1")
            self.claim_at = None
            self.claim_release = now + self.CLAIM_PULSE
        elif self.claim_release is not None and now >= self.claim_release:
            self.send("btn a 0")
            self.claim_release = None
        if self.scroll_until is not None and now >= self.scroll_until:
            self.send("scroll 0 0")
            self.scroll_until = None
        if self.active and now - self.last_used > self.idle and now >= self.gaze_awake_until:
            self.send("hide")
            self.active = False
            log("pointer off (idle)")

    def timeout(self):
        pending = (self.scroll_until, self.claim_at, self.claim_release, self.system_at, self.system_release)
        return 0.02 if any(t is not None for t in pending) else 0.5


def desktop_mouse_present(nodes):
    """Only a physical mouse actually routed by this relay owns the desktop."""
    return any(n.is_mouse and n.role == "pointer" and n.grabbed for n in nodes)


def ipc_request(address, command, timeout=.2):
    """Bounded local request/reply, with no OpenVR client or desktop wake."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as sock:
        sock.bind("")
        sock.settimeout(timeout)
        sock.sendto(command.encode(), address)
        reply = sock.recv(8192).decode()
    if reply.startswith("error"):
        raise RuntimeError(reply)
    return reply


class DesktopMouse:
    """Physical mouse -> nested KDE seat. Never sends to the VR pointer helper."""
    def __init__(self, speed=1):
        if not math.isfinite(speed) or not .05 <= speed <= 10:
            raise ValueError("Desktop mouse speed must be between .05 and 10")
        self.speed = speed
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.dx = self.dy = 0
        self.wheel = [0, 0]
        self.hires = [0, 0]
        self.has_hires = [False, False]
        self.held = set()
        self.sent = self.failed = 0

    def send(self, command):
        try:
            self.sock.sendto(command.encode(), SCREENS)
            self.sent += 1
        except OSError:
            self.failed += 1

    def motion(self, code, value):
        if code == REL_X: self.dx += value
        elif code == REL_Y: self.dy += value
        elif code in (REL_HWHEEL, REL_WHEEL): self.wheel[code == REL_WHEEL] += value
        elif code in (12, 11):
            axis = int(code == 11)
            self.hires[axis] += value
            self.has_hires[axis] = True

    def flush(self):
        if self.dx or self.dy:
            self.send(f"mouse-move {self.dx*self.speed:.6f} {self.dy*self.speed:.6f}")
            self.dx = self.dy = 0
        wheel = [self.hires[i]/120 if self.has_hires[i] else self.wheel[i] for i in (0, 1)]
        if any(wheel): self.send(f"mouse-wheel {wheel[0]:.6f} {wheel[1]:.6f}")
        self.wheel = [0, 0]; self.hires = [0, 0]; self.has_hires = [False, False]

    def button(self, code, value):
        if not BTN_LEFT <= code <= 279 or value not in (0, 1): return
        self.flush()
        self.send(f"mouse-button {code} {value}")
        if value: self.held.add(code)
        else: self.held.discard(code)

    def release_all(self):
        self.dx = self.dy = 0
        self.wheel = [0, 0]; self.hires = [0, 0]; self.has_hires = [False, False]
        for code in sorted(self.held): self.send(f"mouse-button {code} 0")
        self.held.clear()


class Node:
    """One input event node: a candidate device (mouse or keyboard, USB or Bluetooth),
    or any other device with volume keys (candidate False, role "volume")."""

    def __init__(self, path, fd, name, bus, vendor, product, uniq, is_mouse, is_keyboard,
                 candidate=True, volume_keys=False, only_volume=False, uinput=False):
        self.path, self.fd, self.name = path, fd, name
        self.uinput = uinput  # made by a program (frame-voice's keyboard, say), not a real device
        self.bus, self.vendor, self.product, self.uniq = bus, vendor, product, uniq
        self.is_mouse, self.is_keyboard = is_mouse, is_keyboard
        self.candidate, self.volume_keys, self.only_volume = candidate, volume_keys, only_volume
        # One physical device, whatever its node: Bluetooth address, else USB ids plus name.
        base = self.name.split(" Mouse")[0].split(" Keyboard")[0]
        self.id = uniq.lower() if uniq else f"usb:{vendor:04x}:{product:04x}:{base}"
        self.role = None if candidate else "volume"
        self.grabbed = False
        self.remapped = False  # volume keys remapped to their stand-ins
        self.held = set()  # keys and buttons currently down, released if the device vanishes
        self.last_watch = 0.0

    def describe(self):
        kinds = [k for k, on in (("mouse", self.is_mouse), ("keyboard", self.is_keyboard)) if on]
        return {"path": self.path, "name": self.name, "id": self.id, "uniq": self.uniq,
                "bus": {BUS_USB: "usb", BUS_BLUETOOTH: "bluetooth"}.get(self.bus, str(self.bus)),
                "kinds": kinds, "role": self.role, "grabbed": self.grabbed, "uinput": self.uinput}


def probe(path):
    """Open a node if it is a USB or Bluetooth mouse or keyboard, or has volume keys,
    else return None."""
    try:
        fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
    except OSError:
        return None
    try:
        buf = bytearray(256)
        fcntl.ioctl(fd, eviocgname(len(buf)), buf)
        name = buf.split(b"\0", 1)[0].decode(errors="replace")
        ident = bytearray(8)
        fcntl.ioctl(fd, EVIOCGID, ident)
        bus, vendor, product, _ = struct.unpack("HHHH", ident)
        if name.startswith(VIRTUAL_PREFIX):
            raise ValueError
        uniq_buf = bytearray(64)
        try:
            fcntl.ioctl(fd, eviocguniq(len(uniq_buf)), uniq_buf)
            uniq = uniq_buf.split(b"\0", 1)[0].decode(errors="replace")
        except OSError:
            uniq = ""
        keys = bits(fd, EV_KEY, KEY_MAX + 1)
        is_mouse = REL_X in bits(fd, EV_REL, REL_MAX + 1)
        is_keyboard = KEY_A in keys
        candidate = bus in (BUS_USB, BUS_BLUETOOTH) and (is_mouse or is_keyboard)
        volume_keys = bool(keys & VOLUME_CODES)  # stand-ins too: kept from before a relay restart
        if not (candidate or volume_keys):
            raise ValueError
        # uinput devices live here (Bluetooth LE ones come through uhid, under virtual/misc).
        sysfs = os.path.realpath(f"/sys/class/input/{os.path.basename(path)}/device")
        return Node(path, fd, name, bus, vendor, product, uniq, is_mouse, is_keyboard,
                    candidate, volume_keys, keys <= VOLUME_CODES, sysfs.startswith("/sys/devices/virtual/input/"))
    except (OSError, ValueError):
        os.close(fd)
        return None


def main():
    can_grab = "--no-grab" not in sys.argv
    stored = stored_fds()
    mouse = Virtual(f"{VIRTUAL_PREFIX} mouse", 1,
                    keys=range(BTN_MISC, 0x118), rels=range(REL_MAX + 1), stored=stored)
    keyboard = Virtual(f"{VIRTUAL_PREFIX} keyboard", 2,
                       keys=range(1, BTN_MISC), rels=(), stored=stored)
    notify("READY=1")

    control = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    control.bind("\0frametop_relay")
    control.setblocking(False)
    watchers = {}  # address -> watch end time

    # desktop_until: typing goes to the Frametop desktop until then (ft-screens says so
    # every second); typing_applied: the grabs match that as of the last apply_roles().
    # vr_capture_until: every controller button is taken until then (the settings app capturing one).
    state = {"pointer": None, "rules": {}, "meta_dashboard": False, "share_keys": False,
             "desktop_until": 0.0, "typing_applied": None, "vr_capture_until": 0.0,
             "desktop_mouse": None, "mouse_mode": "spatial", "desktop_speed": 1.0,
             "controller_fallback": True}

    def load_config():
        conf = read_config()
        state["rules"] = read_rules()
        state["meta_dashboard"] = conf.get("META_DASHBOARD", "0") == "1"
        state["share_keys"] = conf.get("SHARE_KEYS", "0") == "1"
        state["mouse_mode"] = "desktop" if conf.get("MOUSE_MODE") == "desktop" else "spatial"
        state["controller_fallback"] = conf.get("DESKTOP_CONTROLLER_FALLBACK", "1") == "1"
        try:
            speed = float(conf.get("DESKTOP_MOUSE_SPEED", "1"))
            state["desktop_speed"] = speed if math.isfinite(speed) and .05 <= speed <= 10 else 1
        except ValueError:
            state["desktop_speed"] = 1
        if conf.get("POINTER", "0") == "1":
            p = state["pointer"] or Pointer(0.03, 30)
            p.sensitivity = float(conf.get("POINTER_SENSITIVITY", "0.03"))
            p.idle = float(conf.get("POINTER_IDLE", "30"))
            p.wake_counts = int(conf.get("POINTER_WAKE_COUNTS", "40"))
            state["pointer"] = p
            log(f"pointer mode: {p.sensitivity} deg/count, idle {p.idle} s, wake {p.wake_counts} counts")
        else:
            state["pointer"] = None
            log("pointer mode off: pointer devices feed the virtual mouse and keyboard")

    load_config()
    meta_down = False  # Meta pressed with no other key yet: a tap toggles the dashboard
    screens_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)
    last_typing = 0.0  # the helper was last told of a key then (see "typing" at the top)

    def vr_bind(now):
        """Tell the pointer helper which controller buttons to take from games."""
        if state["desktop_mouse"]:
            buttons = "-"  # leave every physical controller button to SteamVR/games
        elif now < state["vr_capture_until"]:
            buttons = "*"
        else:
            state["vr_capture_until"] = 0.0
            buttons = " ".join(b for b, a in state["rules"]["controller_buttons"].items()
                               if b in VR_BUTTONS and known_action(a) and a not in ("key", "none")
                               and a not in GAZE_ACTIONS) or "-"
            if state["rules"].get("controller_in_games"):
                buttons = "+games " + buttons
        try:
            screens_sock.sendto(f"vrbind {buttons}".encode(), HELPER)
        except OSError:
            pass  # helper not running; it says vrhello when it starts

    def vr_button(button, value, now):
        """A Frame controller button from the pointer helper."""
        for addr, until in list(watchers.items()):
            if now > until:
                del watchers[addr]
            else:
                reply(addr, {"t": "event", "id": VR_DEVICE, "path": "", "name": "Steam Frame controllers",
                             "type": "vr", "code": button, "value": value})
        action = state["rules"]["controller_buttons"].get(button)
        if not state["desktop_mouse"] and (state["pointer"] or (action and not needs_pointer(action))) and known_action(action) \
                and action not in ("key", "none") and action not in GAZE_ACTIONS:
            do_action(action, value, now, button.split("/")[0])

    def vr_keyboard_mode():
        mode = state["rules"].get("vr_keyboard")
        return mode if mode in VR_KEYBOARD_MODES else "no_keyboard"

    def vr_keyboard(command):
        """Open or close Frametop's keyboard (ft-screens)."""
        try:
            screens_sock.sendto(f"vrkeyboard {command}".encode(), SCREENS)
        except OSError:
            pass  # ft-screens not running

    def text_field(focused):
        """A text field on the desktop gained or lost keyboard focus (the input method)."""
        mode = vr_keyboard_mode()
        if not focused:
            if not state["rules"].get("vr_keyboard_persist", True):
                vr_keyboard("hide")  # ft-screens closes it only if it opened it for a text field
        elif mode == "always" or (mode == "no_keyboard" and not any(
                n.candidate and n.is_keyboard and n.role == "passthrough" and not n.uinput for n in nodes.values())):
            vr_keyboard("show")

    def do_action(action, value, now, source="mouse"):
        """A mapped mouse or controller button, or key combination (pointer mode only, but
        for FLOAT_ACTIONS and profiles)."""
        if action == "keyboard_toggle":
            if value == 1 and vr_keyboard_mode() != "never":
                vr_keyboard("toggle")
        elif action.startswith(PROFILE):
            if value == 1:
                # Runs a few seconds and borrows the pointer, like layout_reset.
                subprocess.Popen([FT_LAYOUT, "use", action[len(PROFILE):]], stdin=subprocess.DEVNULL,
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
                log(action)
        elif action in FLOAT_ACTIONS:
            if value == 1:
                try:
                    screens_sock.sendto(FLOAT_ACTIONS[action], FLOAT)
                except OSError:
                    pass  # the Frametop desktop isn't running
                log(action)
        elif state["pointer"]:
            state["pointer"].action(action, value, now, source)

    held_modifiers = set()  # on any keyboard, folded (MODIFIERS)
    held_meta = set()  # the Meta keys held, as they are (KEY_LEFTMETA, KEY_RIGHTMETA)
    meta_hidden = set()  # held Meta keys the desktop was told came up (a combination; key_binding)
    combos_down = {}  # key code -> the action its combination started (released with it)

    def key_binding(code, value, now):
        """A key from a keyboard: does it complete a key combination ("key_bindings")? True if
        it was taken for one (then it isn't typed)."""
        nonlocal meta_down
        if code in MODIFIERS:
            (held_modifiers.add if value else held_modifiers.discard)(MODIFIERS[code])
            if code in (KEY_LEFTMETA, KEY_RIGHTMETA):
                (held_meta.add if value else held_meta.discard)(code)
                if value == 0 and code in meta_hidden:
                    meta_hidden.discard(code)
                    return True  # the desktop already had it come up (below)
            return False
        if value == 1 and code not in combos_down and meta_hidden:
            # Another key while Meta is still held after a combination: the desktop gets Meta
            # back first, so Meta+that key still works there.
            for c in sorted(meta_hidden):
                to_screens(c, 1)
            meta_hidden.clear()
        if value == 0 and code in combos_down:
            action = combos_down.pop(code)
            if state["pointer"] or not needs_pointer(action):
                do_action(action, 0, now, "keyboard")
            return True
        if value != 1 or not state["rules"]["key_bindings"]:
            return value == 2 and code in combos_down
        combo = "+".join(str(c) for c in sorted(held_modifiers) + [code])
        action = state["rules"]["key_bindings"].get(combo)
        if not known_action(action) or action in ("key", "none"):
            return False
        combos_down[code] = action
        if held_meta - meta_hidden:
            # The desktop saw Meta go down. Another key in between keeps its release from
            # opening Plasma's launcher (and Meta from toggling the dashboard here), and Meta
            # comes up there now: KWin takes Meta with a mouse button for moving or resizing
            # windows, which would swallow a gaze click. Its real release is dropped (above).
            meta_down = False
            to_screens(KEY_F24, 1)
            to_screens(KEY_F24, 0)
            for c in sorted(held_meta - meta_hidden):
                to_screens(c, 0)
            meta_hidden.update(held_meta)
        if state["pointer"] or not needs_pointer(action):
            do_action(action, 1, now, "keyboard")
        log(f"key combination {combo}: {action}")
        return True


    screens_down = set()  # keys the desktop was told went down and not yet up (see reconcile_desktop_keys)

    def to_screens(code, value):
        """A key for the desktop screens (ft-screens decides whether it types)."""
        if value in (0, 1) and code < BTN_MISC:
            try:
                screens_sock.sendto(f"key {code} {value}".encode(), SCREENS)
            except OSError:
                return  # ft-screens not running
            (screens_down.add if value else screens_down.discard)(code)
    nodes = {}  # fd -> Node
    # Nodes already probed (rejected or open): path -> inode. A device that disconnects and
    # reconnects between two scans often gets the same event numbers back, so the path alone
    # would hide it; the re-created node has a new inode.
    seen = {}
    next_scan = 0.0

    volume = Volume()
    keys_sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_NONBLOCK)

    def share_key(node, code, value):
        """With SHARE_KEYS=1, a key from a keyboard grabbed for the desktop, for programs
        that watch every keyboard for a hotkey and lose it to the grab. It can't go on
        another input device: gamescope reads every keyboard itself and would type it
        into its focused app."""
        if not state["share_keys"]:
            return
        try:
            keys_sock.sendto(f"key {code} {value} {node.name}".encode(), KEYS)
        except OSError:
            pass  # nobody listening

    def restore_keymaps():
        """Give remapped devices their volume keys back, so they work without the relay."""
        for node in nodes.values():
            if node.remapped:
                try:
                    remap_volume(node.fd, restore=True)
                except OSError:
                    pass  # device already gone

    atexit.register(restore_keymaps)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))  # so atexit runs on systemctl stop

    def take_volume(node):
        """Keep the node's volume keys from gamescope and SteamVR, which read every device.

        Returns False for a non-candidate node there's nothing to do with.
        """
        if not can_grab:
            return node.candidate
        try:
            found = remap_volume(node.fd)
        except OSError as e:
            log(f"remapping volume keys failed for {node.name}: {e}")
            # Some entries may have changed already: handle their stand-ins and restore them.
            node.remapped = True
            found = None
        if found:
            node.remapped = True
            log(f"{node.name} ({node.path}): volume keys taken over (remapped)")
            return True
        if found is None and node.only_volume:
            try:
                fcntl.ioctl(node.fd, EVIOCGRAB, 1)
                node.grabbed = True
                log(f"{node.name} ({node.path}): volume keys taken over (grabbed)")
                return True
            except OSError as e:
                log(f"grab failed for {node.name}: {e}")
        # Grabbed pointer devices still have their volume keys handled here.
        log(f"{node.name} ({node.path}): can't take over its volume keys")
        return node.candidate

    def role_of(node):
        rule = state["rules"]["devices"].get(node.id, {})
        if rule.get("role") in ("pointer", "passthrough", "ignore"):
            return rule["role"]
        has_mouse = any(n.is_mouse for n in nodes.values() if n.id == node.id) or node.is_mouse
        return "pointer" if has_mouse else "passthrough"

    def release_held(node):
        if node.role == "pointer" and state["desktop_mouse"]:
            state["desktop_mouse"].release_all()
            for code in node.held:
                if code < BTN_MISC: to_screens(code, 0)
            node.held.clear()
            return
        for code in node.held:
            if node.role == "passthrough":
                share_key(node, code, 0)
            else:
                (mouse if code >= BTN_MISC else keyboard).emit(EV_KEY, code, 0)
        node.held.clear()
        mouse.sync()
        keyboard.sync()

    def physically_down():
        """The keys down on every device read here, as the kernel has them (EVIOCGKEY)."""
        down = set()
        for node in nodes.values():
            buf = bytearray((KEY_MAX + 8) // 8)
            try:
                fcntl.ioctl(node.fd, EVIOCGKEY, buf)
            except OSError:
                continue
            down.update(i * 8 + bit for i, b in enumerate(buf) if b for bit in range(8) if b >> bit & 1)
        return down

    def reconcile_desktop_keys():
        """A key the desktop has down that no device holds comes up there, and the key
        combinations forget a Meta or modifier no device holds. A key can be left down when
        its device vanishes with it held (release_held only lets go of it here) or a release
        goes astray: on 2026-10-01, after a calibration, Meta stayed down in KWin, so typing
        opened the overview and clicks on the desktop did other things."""
        if not screens_down and not held_meta and not held_modifiers:
            return
        down = physically_down()
        for code in sorted(screens_down - down):
            to_screens(code, 0)
            log(f"key {code} released on the desktop: no keyboard holds it")
        for code in [c for c in held_meta if c not in down]:
            held_meta.discard(code)
            meta_hidden.discard(code)
        held_modifiers.intersection_update({MODIFIERS[c] for c in down if c in MODIFIERS})

    def keys_down(node):
        buf = bytearray((KEY_MAX + 8) // 8)
        try:
            fcntl.ioctl(node.fd, EVIOCGKEY, buf)
        except OSError:
            return False
        return any(buf)

    def apply_roles():
        """Grab pointer devices, and pass-through keyboards while typing goes to the desktop.

        A keyboard with a key down keeps its grab state until it's released, or the key
        would stay held on one side. Returns True if one is still waiting.
        """
        desktop = time.monotonic() < state["desktop_until"]
        waiting = False
        for node in nodes.values():
            if not node.candidate:
                continue  # volume keys only, taken over when found
            role = role_of(node)
            want_grab = can_grab and (role == "pointer"
                                      or (role == "passthrough" and node.is_keyboard and desktop))
            if want_grab != node.grabbed and role == "passthrough" and keys_down(node):
                waiting = True
            elif want_grab != node.grabbed:
                try:
                    fcntl.ioctl(node.fd, EVIOCGRAB, 1 if want_grab else 0)
                    node.grabbed = want_grab
                except OSError as e:
                    log(f"{'grab' if want_grab else 'ungrab'} failed for {node.name}: {e}")
                if not node.grabbed:
                    release_held(node)
            if role != node.role:
                log(f"{node.name} ({node.path}, {node.id}): {role}{', grabbed' if node.grabbed else ''}")
                node.role = role
        if not waiting and desktop != state["typing_applied"]:
            state["typing_applied"] = desktop
            log(f"typing goes to {'the desktop (keyboards grabbed)' if desktop else 'Steam'}")
        return waiting

    def drop(node, reason):
        release_held(node)
        if volume.held and volume.held[0] == node.fd:
            volume.release()
        os.close(node.fd)
        del nodes[node.fd]
        seen.pop(node.path, None)
        log(f"released {node.name} ({node.path}): {reason}")

    def reply(addr, obj):
        try:
            control.sendto(json.dumps(obj).encode(), addr)
        except OSError:
            watchers.pop(addr, None)

    def set_mouse_mode(mode, now):
        if any(keys_down(n) for n in nodes.values() if n.role == "pointer"):
            raise RuntimeError("Release mouse buttons before switching modes")
        native = state["desktop_mouse"]
        if native and native.held:
            raise RuntimeError("Release mouse buttons before switching modes")
        if mode == "desktop":
            ipc_request(SCREENS, "mouse-mode desktop")
            try:
                ipc_request(HELPER, "desktopmouse on")
            except (OSError, RuntimeError):
                ipc_request(SCREENS, "mouse-mode spatial")
                raise
            if native:
                state["mouse_mode"] = mode
                return
            state["desktop_mouse"] = DesktopMouse(state["desktop_speed"])
            if state["pointer"]:
                state["pointer"].active = False
                state["pointer"].dx = state["pointer"].dy = 0
                state["pointer"].claim_at = state["pointer"].claim_release = None
                state["pointer"].scroll_until = state["pointer"].system_at = state["pointer"].system_release = None
        else:
            # An absent desktop must not prevent explicit spatial recovery.
            try: ipc_request(SCREENS, "mouse-mode spatial")
            except OSError: pass
            ipc_request(HELPER, "desktopmouse off")
            if native:
                native.release_all(); native.sock.close()
            state["desktop_mouse"] = None
        state["mouse_mode"] = mode
        vr_bind(now)
        log(f"mouse mode: {mode}")

    def handle_control(now):
        while True:
            try:
                data, addr = control.recvfrom(4096)
            except BlockingIOError:
                return
            words = data.decode(errors="replace").split()
            cmd = words[0] if words else ""
            # One malformed datagram must not end the relay: it would drop every grab,
            # including the volume keys that keep gamescope from aborting. The control
            # socket is an abstract socket, so any local process can send to it.
            try:
                if cmd == "keyboard":
                    # From ft-screens (unbound, no reply): where typing goes, repeated every second.
                    desktop = len(words) > 1 and words[1] == "desktop"
                    state["desktop_until"] = now + 3.0 if desktop else 0.0
                    continue
                if cmd == "vrbtn" and len(words) == 3 and words[1] in VR_BUTTONS and words[2] in ("0", "1"):
                    vr_button(words[1], int(words[2]), now)
                    continue
                if cmd == "vrhello":
                    vr_bind(now)
                    continue
                if cmd == "textfield" and len(words) == 2:
                    text_field(words[1] == "1")
                    continue
                if cmd == "gazeawake" and len(words) == 2:
                    if state["pointer"]:
                        state["pointer"].gaze_awake_until = now + 12.0 if words[1] == "1" else 0.0
                    continue
                if not addr:
                    continue  # unbound sender, nowhere to reply
                if cmd == "devices":
                    reply(addr, {"t": "devices", "pointer_mode": state["pointer"] is not None,
                                 "actions": ACTIONS,
                                 "nodes": [n.describe() for n in nodes.values() if n.candidate]})
                elif cmd == "mouse-mode?":
                    native = state["desktop_mouse"]
                    reply(addr, {"supported": True, "mode": "desktop" if native else "spatial",
                                 "requestedMode": state["mouse_mode"], "speed": state["desktop_speed"],
                                 "heldButtons": sorted(native.held) if native else [],
                                 "sent": native.sent if native else 0, "sendFailures": native.failed if native else 0})
                elif cmd == "mouse-mode" and len(words) == 2 and words[1] in ("desktop", "spatial"):
                    try:
                        set_mouse_mode(words[1], now)
                        reply(addr, {"mode": words[1], "ok": True})
                    except (OSError, RuntimeError, ValueError) as error:
                        reply(addr, {"error": str(error), "ok": False})
                elif cmd == "watch":
                    seconds = float(words[1]) if len(words) > 1 else 30
                    watchers[addr] = now + min(seconds, 600)
                    reply(addr, {"t": "watching", "seconds": seconds})
                elif cmd == "reload":
                    load_config()
                    apply_roles()
                    if state["pointer"]:
                        state["pointer"].send("reload")
                    vr_bind(now)
                    reply(addr, {"t": "reloaded"})
                elif cmd == "vrcapture":
                    seconds = float(words[1]) if len(words) > 1 else 30
                    state["vr_capture_until"] = now + min(seconds, 120) if seconds > 0 else 0.0
                    vr_bind(now)
                    reply(addr, {"t": "vrcapture", "seconds": seconds})
                else:
                    reply(addr, {"t": "error", "error": f"unknown command {cmd!r}"})
            except Exception as e:
                log(f"bad control datagram {data!r}: {e!r}")

    def broadcast(node, etype, code, value, now):
        if not watchers or not node.candidate:
            return
        if etype == EV_REL and now - node.last_watch < 0.05:
            return  # motion: enough for an activity light
        node.last_watch = now
        msg = {"t": "event", "id": node.id, "path": node.path, "name": node.name,
               "type": EV_NAMES.get(etype, str(etype)), "code": code, "value": value}
        for addr, until in list(watchers.items()):
            if now > until:
                del watchers[addr]
            else:
                reply(addr, msg)

    vr_bind(time.monotonic())  # a helper that's already running keeps its buttons in step
    waiting = False  # a keyboard's grab waits for its keys to come up
    # A relay that went away with a key down left it down on the desktop, where this one
    # never sent it: modifiers come up there now (a release of a key that isn't down is nothing).
    for code in sorted(MODIFIERS):
        to_screens(code, 0)
    while True:
        now = time.monotonic()
        pointer = state["pointer"]
        if now >= next_scan:
            next_scan = now + 1.0
            reconcile_desktop_keys()
            if (state["mouse_mode"] == "desktop") != bool(state["desktop_mouse"]):
                try: set_mouse_mode(state["mouse_mode"], now)
                except (OSError, RuntimeError, ValueError): pass  # desktop may be starting; retry on the next scan
            current = {}
            for name in os.listdir("/dev/input"):
                if name.startswith("event"):
                    try:
                        current[f"/dev/input/{name}"] = os.stat(f"/dev/input/{name}").st_ino
                    except OSError:
                        pass
            for path in [p for p in seen if p not in current]:
                del seen[path]
            added = False
            for path, ino in sorted(current.items()):
                if seen.get(path) == ino:
                    continue
                # New here: a new device, or one that came back in the same place.
                for old in [n for n in nodes.values() if n.path == path]:
                    drop(old, "replaced by a new device node")
                seen[path] = ino
                node = probe(path)
                if node and node.volume_keys and not take_volume(node):
                    os.close(node.fd)
                    node = None
                if node:
                    nodes[node.fd] = node
                    added = True
            if added:
                apply_roles()
            # Presence, not activity: an idle connected mouse keeps its own seat.
            present = desktop_mouse_present(nodes.values())
            try:
                ipc_request(SCREENS, "mouse-presence %d %d" %
                            (present, state["controller_fallback"]))
            except (OSError, RuntimeError, ValueError):
                pass  # Older/missing compositor: preserve existing native mode.

        ready, _, _ = select.select(list(nodes) + [control], [], [],
                                    volume.timeout(now, pointer.timeout() if pointer else 0.5))
        now = time.monotonic()
        if pointer and not state["desktop_mouse"]:
            pointer.tick(now)
        volume.tick(now)
        if state["vr_capture_until"] and now >= state["vr_capture_until"]:
            vr_bind(now)  # capture over: back to the mapped buttons
        if (now < state["desktop_until"]) != state["typing_applied"] or waiting:
            waiting = apply_roles()
        for fd in ready:
            if fd is control:
                handle_control(now)
                continue
            node = nodes[fd]
            try:
                data = os.read(fd, EVENT.size * 64)
            except OSError as e:
                if e.errno == errno.EAGAIN:
                    continue
                drop(node, os.strerror(e.errno))
                continue
            if not data:
                drop(node, "closed")
                continue
            buttons = state["rules"]["buttons"].get(node.id, {})
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, etype, code, value = EVENT.unpack_from(data, off)
                if etype in (EV_KEY, EV_REL):
                    broadcast(node, etype, VOLUME_ORIGINAL.get(code, code) if node.remapped else code,
                              value, now)
                if etype == EV_KEY and ((node.remapped and code in VOLUME_ORIGINAL)
                                        or (node.grabbed and code in VOLUME_STANDIN)):
                    volume.key(fd, code, value, now)
                    if value == 1:
                        meta_down = False  # Meta used as a modifier, not a tap
                    continue
                if node.role == "volume":
                    continue
                if node.role != "pointer":
                    # Observed only, unless typing goes to the desktop. With META_DASHBOARD=1,
                    # a Meta tap on any keyboard toggles the dashboard.
                    if node.role == "passthrough" and etype == EV_KEY and code < BTN_MISC and key_binding(code, value, now):
                        continue
                    if node.role == "passthrough" and etype == EV_KEY:
                        if value == 1 and code < BTN_MISC and now - last_typing >= 0.25:
                            last_typing = now
                            screens_sock.sendto(b"typing", HELPER)
                        to_screens(code, value)
                        if node.grabbed and code < BTN_MISC and value in (0, 1):
                            share_key(node, code, value)
                            if value:
                                node.held.add(code)
                            else:
                                node.held.discard(code)
                    if (pointer and not state["desktop_mouse"] and state["meta_dashboard"] and node.role == "passthrough"
                            and etype == EV_KEY):
                        if code in (KEY_LEFTMETA, KEY_RIGHTMETA):
                            if value == 1:
                                meta_down = True
                            elif value == 0 and meta_down:
                                meta_down = False
                                pointer.dashboard()
                        elif value == 1:
                            meta_down = False  # Meta used as a modifier, not a tap
                    continue
                if etype == EV_KEY:
                    if state["desktop_mouse"]:
                        if code >= BTN_LEFT:
                            state["desktop_mouse"].button(code, value)
                        else:
                            to_screens(code, value)
                        if value: node.held.add(code)
                        else: node.held.discard(code)
                        continue
                    action = buttons.get(str(code), DEFAULT_BUTTONS.get(code, "key"))
                    if pointer and action not in ("key", "none"):
                        do_action(action, value, now)
                        continue
                    if action == "none":
                        continue
                    target = mouse if code >= BTN_MISC else keyboard
                    target.emit(etype, code, value)
                    to_screens(code, value)
                    if value:
                        node.held.add(code)
                    else:
                        node.held.discard(code)
                elif etype == EV_REL:
                    if state["desktop_mouse"]:
                        state["desktop_mouse"].motion(code, value)
                    elif pointer:
                        pointer.motion(code, value, now)
                    else:
                        mouse.emit(etype, code, value)
                elif etype == EV_SYN and code == SYN_REPORT:
                    if state["desktop_mouse"]:
                        state["desktop_mouse"].flush()
                    elif pointer:
                        pointer.flush()
                    mouse.sync()
                    keyboard.sync()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
