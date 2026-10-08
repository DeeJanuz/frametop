#!/usr/bin/env python3
"""ft-layout: the Frametop screens' sizes and where they float around you.

Two desktop backends (BACKEND in ~/.config/frametop.conf):
  screens    (default) ft-screens, our own compositor (screens): each screen
             is its own SteamVR panel with its own resolution and size in metres. ft-layout
             places them directly through ft-screens' control socket (@ft_screens).
  gamescope  the old path: every screen one size (at most 1920x1080 worth of pixels),
             panels owned by the SteamVR dashboard, so ft-layout floats each one with
             `vrcmd --dock-overlay` and the pointer helper carries it into place.

The layout is relative to your head when it's applied: its position and the direction
you face (yaw only), like a recenter. It lives in ~/.config/frametop-layout.json:
  {"auto": true,                      arrange when the desktop starts
   "mode": "preset" | "custom",
   "preset": {"kind": "arc" | "flat", "rows": 1, "distance": 2.0, "gap": 0.05, "height": 0},
   "primary": 2,                      the screen with the taskbar (1-based; default: the biggest)
   "layouts": {"Work": [{"pos": ..., "face": ..., "roll": ..., "metres": ..., "curve": ...,
                         "pin": ...}, ...]},   named layouts: each screen's place (SPATIAL)
   "active": "Work",                  the named layout the custom arrangement came from
   "profiles": {"Work": {"hidden": [3], "windows": [...]}},   what a named layout opens too
                                      (docs/profiles.md): screens hidden on their own, and the
                                      apps' windows (ft-floatd's "windows")
   "default_profile": "Work",         the profile the desktop starts with (FT_PROFILE overrides)
   "visibility": {"mode": "always",   ft-screens: always | dashboard (only with the SteamVR
                  "wrist_angle": 60,    dashboard open) | gesture (while you look at a controller)
                  "gesture_hand": "left", "gesture_angle": 20},   | toggle (hidden until shown);
                                      wrist_angle: a pinned screen shows while you see its front
                                      within this many degrees
   "screens": [{"size": [w, h], "metres": 3.6,        ft-screens: pixels, and width in VR
                "curve": 0,                           ft-screens: cylinder radius in metres, 0 = flat
                "pin": {"hand": "left", "rel": [12]},  ft-screens: riding on that controller
                                                      (left | right), or on the headset (head)
                "scale": 1.0,                         KWin output scale (1.0 = 100%)
                "hidden": true,                       ft-screens: hidden on its own, whatever the
                                                      visibility mode (ft-layout hide N)
                "pos": [x, y, z], "face": [yaw, pitch], "roll": 0,   custom layout
                "rotation": "normal" | "left" | "right"}, ...],      gamescope only
   "panel_size": [w, h],              gamescope: last measured panel size
   "hosts": [{"name": "desk-pc", "address": "192.168.1.20", ft-screens: other machines' displays
              "direct": ["10.35.78.20"],          its Steam Link dongle on the Frame's hotspot
              "route": "auto",                    auto (the dongle when it answers) | network |
                                                          dongle (only; ft-stream reads both)
              "displays": [{"id": "desk-pc-oled",         (docs/remote-displays.md), each a panel
                            "client": "frametop-1",       streamed by ft-stream: its paired client,
                            "app": "display:{GUID}",      what it streams (display:DEVICE, monitor:
                                                          a Vibepollo Remote Monitor, primary),
                            "label": "OLED", "size": [5120, 1440], "fps": 60, "bitrate": 50000,
                            "metres": 1.8, "curve": 0, "pin": ..., "hidden": true,
                            "off": true,                  disconnected: no stream, no panel
                            "pos": ..., "face": ..., "roll": 0}]}]}   as a screen's
Remote displays are screens 101 and up, in the order they're listed. Without a place of their
own they go in a row above the screens. A profile keeps the ones connected when it was saved,
like its apps: their places and hidden state by id (profiles[NAME]["remote"]). Opening it, or
arranging while it's the one in use, connects each whose host answers and puts it there; the
others stay as they are.
Custom positions: x right, y up, -z forward from the head, in metres; face = the
direction you look to see the screen's front straight on, in degrees, relative to your
heading; roll = the panel turned about its front, counterclockwise as you see it.
Presets: "arc" hinges the screens edge to edge around you, each turned to face you
(like monitors on a desk); "flat" puts them on one flat wall facing forward. Screen 1
is top left, then left to right.

Usage (on the Frame host; Frametop Display Settings calls it too):
  ft-layout apply [--wait SECONDS]   arrange every screen; --wait is for desktop start:
                                     wait for the screens, skip if "auto" is off
  ft-layout capture                  save the current arrangement as the custom layout
  ft-layout save NAME                save it as a named layout too, and use that; with the
                                     desktop's apps and hidden screens, as a profile
  ft-layout use NAME                 switch to a named layout, arrange the screens in it, and
                                     open its apps (moving open windows, nothing closed)
  ft-layout start [--wait SECONDS]   desktop start: the profile in FT_PROFILE or default_profile,
                                     or else as apply --wait
  ft-layout open NAME                a profile's launcher entry: use it, or start the desktop in it
  ft-layout default NAME|none        the profile the desktop starts with
  ft-layout layouts                  list the named layouts (* = the one in use)
  ft-layout rename OLD NEW | delete NAME
  ft-layout launchers                write each profile's launcher entry again (and drop stale ones)
  ft-layout plan                     print the arrangement as JSON (no VR needed)
  ft-layout scale                    per-screen scale, positions (as the screens are around
                                     you), and primary to KWin
  ft-layout screen-args              ft-screens' --screen arguments for the session script
  ft-layout remote-view              the primary screen's place in the workspace, for the VNC bridge
  ft-layout toggle                   hide or show all screens (ft-screens)
  ft-layout hide N|all               hide a screen on its own (it stays hidden whatever the
  ft-layout show N|all               visibility mode or the hotkey say), or show it again
  ft-layout hidden                   the screens hidden on their own
  ft-layout pin all|N left|right|head  pin screens to a wrist or your head as they are;
                                     unpin all|N
  ft-layout remote list              the remote displays, their numbers and their streams' state
  ft-layout remote monitors ADDRESS  the host's displays (Vibepollo's /api/display-devices, JSON)
  ft-layout remote add HOST ADDRESS APP [--size WxH] [--fps F] [--bitrate KBPS] [--label TEXT]
                                     a display of a host (pairs its client with the host's token
                                     first) -> its id
  ft-layout remote set ID size=WxH|fps=F|bitrate=KBPS|label=TEXT|app=APP|metres=M ...
  ft-layout remote host NAME route=auto|network|dongle direct=ADDRESS[,ADDRESS]|none
                                     how a host's streams reach it; its running streams start over
  ft-layout remote connect ID...     start their streams again (and keep them on)
  ft-layout remote disconnect ID...  stop their streams and panels until connected again
  ft-layout remote remove ID         stops it and unpairs its client (the host stays)
"""
import concurrent.futures
import fcntl
import json
import math
import os
import re
import socket
import subprocess
import sys
import time

LAYOUT_PATH = os.path.expanduser("~/.config/frametop-layout.json")
CONF_PATH = os.path.expanduser("~/.config/frametop.conf")
VRCMD = "/opt/steamvr/bin/linuxarm64/vrcmd"
HELPER = "\0ft_pointer_helper"
SCREENS = "\0ft_screens"
LOCK_PATH = "/tmp/ft-layout.lock"
FLOAT = "\0frametop_float"  # ft-floatd: the apps' windows (profiles)
REPO = os.path.dirname(os.path.dirname(os.path.realpath(__file__)))
LAUNCHERS = os.path.expanduser("~/.local/share/applications")  # a profile's launcher entry each
DEFAULT_PANEL = (1.18, 0.664)  # gamescope: a floating 16:9 dashboard panel, measured on the Frame
PIXELS_PER_METRE = 800         # ft-screens: a new screen's default size in VR (1920 px: 2.4 m)
# controllers: when controllers' lasers work the screens (always | outside_games | dashboard).
# in_games: during a VR game, "always" hides the screens unless the dashboard is open (hide),
# or leaves them up (visible).
VISIBILITY = {"mode": "always", "wrist_angle": 60, "gesture_hand": "left", "gesture_angle": 20,
              "controllers": "outside_games", "in_games": "hide"}
SPATIAL = ("pos", "face", "roll", "metres", "curve", "pin")  # what a named layout keeps of a screen
DEFAULTS = {"auto": True, "mode": "preset",
            "preset": {"kind": "arc", "rows": 1, "distance": 2.0, "gap": 0.05, "height": 0.0},
            "screens": [], "panel_size": list(DEFAULT_PANEL), "hosts": []}
REMOTE_FIRST = 101  # ft-screens' remote screens (screens/remote.h)
REMOTE_MAX = 16
REMOTE_PIXELS_PER_METRE = 2400  # a new remote display's width in VR (5120 px: 2.1 m), at least 1 m
WEB_UI_PORT = 47990  # Vibepollo's Web UI: a host answers there when it's up
STREAM = os.path.join(REPO, "stream", "build", "ft-stream")


def log(*args):
    print(*args, flush=True)


# ---------------------------------------------------------------- config

def read_conf():
    conf = {}
    try:
        with open(CONF_PATH) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if "=" in line:
                    k, v = line.split("=", 1)
                    conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def backend():
    return "gamescope" if read_conf().get("BACKEND", "screens") == "gamescope" else "screens"


def screen_count(layout=None):
    """ft-screens: the configured screens; gamescope: SCREENS."""
    if backend() == "screens":
        return max(1, len((layout or load_layout()).get("screens", [])))
    try:
        return max(1, int(read_conf().get("SCREENS", "2")))
    except ValueError:
        return 2


def load_layout():
    layout = json.loads(json.dumps(DEFAULTS))
    try:
        with open(LAYOUT_PATH) as f:
            saved = json.load(f)
        layout.update({k: v for k, v in saved.items() if k != "preset"})
        layout["preset"].update(saved.get("preset", {}))
    except (OSError, ValueError):
        pass
    if backend() == "screens" and not layout.get("screens"):
        layout["screens"] = [{"size": [1920, 1080], "metres": 1920 / PIXELS_PER_METRE}]
    return layout


def save_layout(layout):
    os.makedirs(os.path.dirname(LAYOUT_PATH), exist_ok=True)
    tmp = LAYOUT_PATH + ".tmp"
    with open(tmp, "w") as f:
        json.dump(layout, f, indent=2)
    os.replace(tmp, LAYOUT_PATH)


def screen_entry(layout, i):
    screens = layout.get("screens", [])
    return screens[i] if i < len(screens) else {}


def screen_scale(layout, i):
    return float(screen_entry(layout, i).get("scale", 1.0))


def screen_pixels(layout, i):
    w, h = screen_entry(layout, i).get("size", [1920, 1080])
    return int(w), int(h)


def screen_metres(layout, i):
    w, _ = screen_pixels(layout, i)
    return float(screen_entry(layout, i).get("metres", w / PIXELS_PER_METRE))


ROLL = {"normal": 0.0, "left": 90.0, "right": -90.0}


def screen_rotation(layout, i):
    if backend() == "screens":
        return "normal"  # portrait screens are simply tall
    r = screen_entry(layout, i).get("rotation", "normal")
    return r if r in ROLL else "normal"


def screen_size(layout, i, panel_size=None):
    """A screen's size in VR (width, height) in metres."""
    if backend() == "screens":
        w, h = screen_pixels(layout, i)
        m = screen_metres(layout, i)
        return m, m * h / w
    w, h = panel_size or layout.get("panel_size") or DEFAULT_PANEL
    return (h, w) if screen_rotation(layout, i) != "normal" else (w, h)


def primary_screen(layout):
    """0-based index of the screen with the taskbar: the chosen one, or the biggest."""
    n = screen_count(layout)
    p = layout.get("primary")
    if isinstance(p, int) and 1 <= p <= n:
        return p - 1
    return max(range(n), key=lambda i: screen_pixels(layout, i)[0] * screen_pixels(layout, i)[1])


# ---------------------------------------------------------------- geometry
# Head frame: x right, y up, -z forward, at the eye, turned to the heading (yaw).
# yaw 0 = -Z, positive yaw turns left, positive pitch looks up (as in SteamVR's helper).

def direction(yaw, pitch):
    y, p = math.radians(yaw), math.radians(pitch)
    return (-math.sin(y) * math.cos(p), math.sin(p), -math.cos(y) * math.cos(p))


def yaw_pitch(v):
    n = math.sqrt(sum(c * c for c in v)) or 1.0
    return math.degrees(math.atan2(-v[0], -v[2])), math.degrees(math.asin(max(-1.0, min(1.0, v[1] / n))))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def normalize(v):
    n = math.sqrt(dot(v, v)) or 1.0
    return tuple(c / n for c in v)


def turn_yaw(v, yaw):
    """Rotate v about +Y by yaw degrees (head frame -> world for the heading yaw)."""
    s, c = math.sin(math.radians(yaw)), math.cos(math.radians(yaw))
    return (v[0] * c + v[2] * s, v[1], -v[0] * s + v[2] * c)


def _chain(widths, d, gap):
    """One row of flat screens hinged edge to edge around the eye, each turned to face it,
    like monitors on a desk: the middle screen (or the seam between the middle two) is
    straight ahead at distance d; each neighbour starts at the previous screen's outer
    edge (plus the gap) and is turned until it faces the eye. Returns [(x, z, yaw)], in
    the head frame from above (x right, -z forward)."""
    n = len(widths)
    out = [None] * n

    def right(yaw):  # a screen's right vector for its yaw
        return math.cos(math.radians(yaw)), -math.sin(math.radians(yaw))

    def yaw_of(x, z):
        return math.degrees(math.atan2(-x, -z))

    def hang(hinge, w, side):
        # Turn the screen about its hinge until it faces the eye: its centre
        # C = hinge + side * w/2 * right(yaw) must have no sideways part, dot(C, right) = 0,
        # that is dot(hinge, right(yaw)) = -side * w/2. Take the root nearest the hinge's
        # own direction, turning outward (right: yaw falls; left: yaw rises).
        def g(yaw):
            rx, rz = right(yaw)
            return hinge[0] * rx + hinge[1] * rz + side * w / 2
        start = yaw_of(*hinge)
        a, ga = start, g(start)
        for k in range(1, 721):  # 0.25-degree steps, up to half a turn
            b = start - side * k * 0.25
            gb = g(b)
            if ga == 0 or ga * gb < 0:
                for _ in range(50):
                    mid = (a + b) / 2
                    if g(a) * g(mid) <= 0:
                        b = mid
                    else:
                        a = mid
                break
            a, ga = b, gb
        yaw = a
        rx, rz = right(yaw)
        return hinge[0] + side * rx * w / 2, hinge[1] + side * rz * w / 2, yaw

    mid = n // 2
    if n % 2:
        out[mid] = (0.0, -d, 0.0)
        right_edge, left_edge, first_right, first_left = (widths[mid] / 2, -d), (-widths[mid] / 2, -d), mid + 1, mid - 1
        yaw_r = yaw_l = 0.0
    else:  # a seam straight ahead
        right_edge, left_edge, first_right, first_left = (gap / 2, -d), (-gap / 2, -d), mid, mid - 1
        yaw_r = yaw_l = 0.0
    for i in range(first_right, n):  # to the right: screen i hangs from the previous right edge
        rx, rz = right(yaw_r)
        g = gap if i != first_right or n % 2 else 0
        hinge = (right_edge[0] + rx * g, right_edge[1] + rz * g)
        cx, cz, yaw_r = hang(hinge, widths[i], +1)
        out[i] = (cx, cz, yaw_r)
        rx, rz = right(yaw_r)
        right_edge = (cx + rx * widths[i] / 2, cz + rz * widths[i] / 2)
    for i in range(first_left, -1, -1):  # to the left, mirrored
        rx, rz = right(yaw_l)
        g = gap if i != first_left or n % 2 else 0
        hinge = (left_edge[0] - rx * g, left_edge[1] - rz * g)
        cx, cz, yaw_l = hang(hinge, widths[i], -1)
        out[i] = (cx, cz, yaw_l)
        rx, rz = right(yaw_l)
        left_edge = (cx - rx * widths[i] / 2, cz - rz * widths[i] / 2)
    return out


def plan(layout, count, panel_size=None):
    """Screen poses in the head frame: [{"pos": (x, y, z), "face": (yaw, pitch), "roll": deg}]."""
    sizes = [screen_size(layout, i, panel_size) for i in range(count)]
    rolls = [ROLL[screen_rotation(layout, i)] for i in range(count)]
    if layout.get("mode") == "custom" and len(layout.get("screens", [])) >= count and all(
            "pos" in s for s in layout["screens"][:count]):
        return [{"pos": tuple(s["pos"]), "face": tuple(s.get("face", yaw_pitch(s["pos"]))),
                 "roll": float(s.get("roll", rolls[i]))} for i, s in enumerate(layout["screens"][:count])]
    p = layout["preset"]
    rows = max(1, min(int(p.get("rows", 1)), count))
    cols = math.ceil(count / rows)
    # A row setting above what the screens fill leaves empty grid rows (4 screens,
    # 3 rows -> cols 2 -> a third row with nothing in it), and max() over an empty
    # row crashes plan(). Trim rows to what the screens actually fill.
    rows = max(1, math.ceil(count / cols))
    d = max(0.3, float(p.get("distance", 2.0)))
    gap = max(0.0, float(p.get("gap", 0.05)))
    height = float(p.get("height", 0.0))
    flat = p.get("kind") == "flat"
    grid = [list(range(r * cols, min(count, (r + 1) * cols))) for r in range(rows)]
    out = [None] * count
    if flat:
        # One flat wall: rows of screens side by side, centred, facing forward.
        row_h = [max(sizes[i][1] for i in row) for row in grid]
        top = height + (sum(row_h) + gap * (rows - 1)) / 2
        for r, row in enumerate(grid):
            y = top - sum(row_h[:r]) - gap * r - row_h[r] / 2
            x = -(sum(sizes[i][0] for i in row) + gap * (len(row) - 1)) / 2
            for i in row:
                out[i] = {"pos": (x + sizes[i][0] / 2, y, -d), "face": (0.0, 0.0), "roll": rolls[i]}
                x += sizes[i][0] + gap
        return out
    # Curved: each row hinged edge to edge around you (see _chain); rows stacked by angle
    # (a row of height h at distance d spans 2 atan(h/2d)), each tilted to face you.
    span = lambda m: 2 * math.degrees(math.atan(m / 2 / d))
    row_h = [max(span(sizes[i][1]) for i in row) for row in grid]
    g = span(gap)
    top = math.degrees(math.atan(height / d)) + (sum(row_h) + g * (rows - 1)) / 2
    for r, row in enumerate(grid):
        pitch = top - sum(row_h[:r]) - g * r - row_h[r] / 2
        cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
        for i, (x, z, yaw) in zip(row, _chain([sizes[i][0] for i in row], d, gap)):
            # Tilt the row about the eye's left-right axis: forward distance shrinks by cos,
            # height grows by sin.
            r_h = math.hypot(x, z)
            out[i] = {"pos": (x * cp, r_h * sp, z * cp), "face": (yaw, pitch), "roll": rolls[i]}
    return out


def relative_pose(center, x_axis, z_axis, eye, heading):
    """A screen's pose in the world -> custom layout entry (pos, face, roll) in the head frame."""
    rel = turn_yaw(tuple(c - e for c, e in zip(center, eye)), -heading)
    fyaw, fpitch = yaw_pitch(tuple(-c for c in z_axis))
    # Roll: the panel's right vector against an upright panel's right and up.
    right = normalize(cross((0.0, 1.0, 0.0), z_axis))
    up = cross(z_axis, right)
    roll = math.degrees(math.atan2(dot(x_axis, up), dot(x_axis, right)))
    return {"pos": [round(v, 4) for v in rel], "face": [round(fyaw - heading, 2), round(fpitch, 2)],
            "roll": round(roll, 2)}


class Socket:
    """Request/reply over an abstract datagram socket (ft-screens or the pointer helper)."""

    def __init__(self, address, what):
        self.address, self.what = address, what
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        self.sock.bind("")  # autobind: an abstract address the other side can reply to

    def ask(self, text, timeout=10.0):
        self.sock.settimeout(timeout)
        try:
            self.sock.sendto(text.encode(), self.address)
            reply = self.sock.recv(8192).decode()
        except (OSError, socket.timeout) as e:
            raise RuntimeError(f"{self.what} didn't answer ({e})")
        if not reply.startswith("ok"):
            raise RuntimeError(reply)
        return reply


# ---------------------------------------------------------------- ft-screens

def screens_socket():
    return Socket(SCREENS, "ft-screens (the desktop's compositor) isn't running or")


def screen_args(layout=None):
    layout = layout or load_layout()
    return " ".join(f"--screen {w}x{h}@{screen_metres(layout, i):.3f}"
                    for i, (w, h) in ((i, screen_pixels(layout, i)) for i in range(screen_count(layout))))


def visibility(layout):
    v = dict(VISIBILITY)
    v.update(layout.get("visibility", {}))
    return v


def send_visibility(sock, layout):
    v = visibility(layout)
    sock.ask(f"visibility {v['mode']}")
    sock.ask(f"wrist {float(v['wrist_angle']):.1f}")
    sock.ask(f"gesture {v['gesture_hand']} {float(v['gesture_angle']):.1f}")
    sock.ask(f"controllers {v['controllers']}")
    sock.ask(f"ingames {v['in_games']}")


def send_hidden(sock, layout):
    """Screens hidden on their own (ft-screens' conceal/reveal)."""
    for i in range(screen_count(layout)):
        word = "conceal" if screen_entry(layout, i).get("hidden") else "reveal"
        try:
            sock.ask(f"{word} {i + 1}")
        except RuntimeError as e:
            log(f"screens hidden on their own: {e}")  # an ft-screens from before conceal
            return


def set_hidden(which, hidden):
    """Hide (or show) screen N (1-based) or "all" on its own: saved, and applied if the
    desktop runs."""
    layout = load_layout()
    n = screen_count(layout)
    remotes = {num: d for num, _, d in remote_displays(layout)}
    if which == "all":
        picked, picked_remote = range(n), list(remotes)
    elif which.isdigit() and int(which) in remotes:
        picked, picked_remote = [], [int(which)]
    else:
        picked, picked_remote = ([int(which) - 1] if which.isdigit() else []), []
    if not (picked or picked_remote) or not all(0 <= i < n for i in picked):
        raise RuntimeError(f"no screen {which} (1 to {n}, or a remote display's number)")
    for num in picked_remote:
        if hidden:
            remotes[num]["hidden"] = True
        else:
            remotes[num].pop("hidden", None)
    screens = layout.setdefault("screens", [])
    while len(screens) < n:
        screens.append({})
    for i in picked:
        if hidden:
            screens[i]["hidden"] = True
        else:
            screens[i].pop("hidden", None)
    save_layout(layout)
    try:
        sock = screens_socket()
        for num in [i + 1 for i in picked] + picked_remote:
            sock.ask(f"{'conceal' if hidden else 'reveal'} {num}")
    except RuntimeError as e:
        log(f"saved; not applied now: {e}")


def parse_get(reply):
    """ft-screens' "get": pose, size, curve, and the pin (hand and controller->screen)."""
    f = reply.split()[1:]
    g = list(map(float, f[:15]))
    out = {"center": tuple(g[0:3]), "x": tuple(g[3:6]), "y": tuple(g[6:9]), "z": tuple(g[9:12]),
           "metres": g[12], "height": g[13], "curve": g[14], "hand": f[15] if len(f) > 15 else "none"}
    if out["hand"] != "none" and len(f) >= 28:
        out["rel"] = [round(float(v), 5) for v in f[16:28]]
    return out


def screens_up(sock):
    """How many screens ft-screens has shown so far."""
    f = sock.ask("screens").split()
    return sum(1 for s in f[2:] if not s.split(":")[1].startswith("0x"))


def apply_screens(wait=0):
    layout = load_layout()
    count = screen_count(layout)
    deadline = time.time() + wait
    while True:
        try:
            sock = screens_socket()
            if screens_up(sock) >= count or time.time() >= deadline:
                break
        except RuntimeError:
            if time.time() >= deadline:
                raise
        time.sleep(1)
    start_remotes(sock, layout)
    save_layout(layout)  # screen numbers given to new remote displays
    f = sock.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    send_visibility(sock, layout)
    results = []
    for i, t in enumerate(plan(layout, count)):
        world = turn_yaw(t["pos"], heading)
        center = tuple(e + v for e, v in zip(eye, world))
        entry = screen_entry(layout, i)
        sock.ask(f"width {i + 1} {screen_metres(layout, i):.4f}")
        sock.ask(f"curve {i + 1} {float(entry.get('curve', 0)):.3f}")
        results.append(sock.ask("place %d %.4f %.4f %.4f %.3f %.3f %.3f" % (i + 1, *center, t["face"][0] + heading,
                                                                             t["face"][1], t["roll"])))
        pin = entry.get("pin") if layout.get("mode") == "custom" else None
        if pin and len(pin.get("rel", [])) == 12:
            try:
                sock.ask(f"pin {i + 1} {pin['hand']} " + " ".join(f"{v:.5f}" for v in pin["rel"]))
            except RuntimeError as e:
                log(f"screen {i + 1}: {e}")  # that controller isn't on
    send_hidden(sock, layout)
    place_remotes(sock, layout, count, eye, heading)
    try:
        sock.ask("vrkeyboard close")  # the keyboard, if open, goes too: a reset starts over
    except RuntimeError:
        pass  # an older ft-screens
    log(f"arranged {count} screen(s)")
    return results


def capture_screens():
    layout = load_layout()
    sock = screens_socket()
    f = sock.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    screens = []
    for i in range(screen_count(layout)):
        g = parse_get(sock.ask(f"get {i + 1}"))
        entry = dict(screen_entry(layout, i))
        entry.update(relative_pose(g["center"], g["x"], g["z"], eye, heading))
        entry["metres"] = round(g["metres"], 4)  # resized by hand
        entry["curve"] = round(g["curve"], 3)
        entry.pop("pin", None)
        if "rel" in g:
            entry["pin"] = {"hand": g["hand"], "rel": g["rel"]}
        screens.append(entry)
    layout["screens"] = screens + layout.get("screens", [])[len(screens):]
    layout["mode"] = "custom"
    capture_remotes(sock, layout, eye, heading)
    save_layout(layout)
    return screens


# ---------------------------------------------------------------- gamescope (dashboard panels)

def vrcmd(*args, timeout=10):
    # SteamVR's config folder: in the desktop, XDG_CONFIG_HOME is its own (docs/design.md).
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.dirname(VRCMD), XDG_CONFIG_HOME=os.path.expanduser("~/.config"))
    try:
        return subprocess.run([VRCMD, *args], capture_output=True, text=True, timeout=timeout, env=env).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""


def screen_keys():
    """The screens' overlay keys, in window order. gamescope (PerWindow) names each
    window's overlay frametop.app.<window seq>; .app.0 is its default connector, which
    never gets a window. It must be skipped: docking an unknown key moves whatever
    panel the dashboard shows instead."""
    keys = []
    for m in re.finditer(r"^'(frametop\.app\.(\d+))' .*VROverlayType_Dashboard_Main", vrcmd("--overlays"), re.M):
        if int(m.group(2)) > 0:
            keys.append((int(m.group(2)), m.group(1)))
    return [k for _, k in sorted(keys)]


def visible_keys():
    return set(re.findall(r"^'([^']+)' .* visible VROverlayType", vrcmd("--overlays"), re.M))


def helper_measure(helper, key):
    f = list(map(float, helper.ask(f"measure {key}").split()[1:]))
    return {"center": tuple(f[0:3]), "size": (f[3], f[4]), "x": tuple(f[5:8]), "y": tuple(f[8:11]),
            "z": tuple(f[11:14])}


def float_screen(key):
    """Float a docked screen: dock it into the dashboard (which opens the dashboard; a
    floated panel takes its first position from it, and none exists while it's closed),
    float it, then close the dashboard (so the carry can't snap it back in). A new
    window starts docked in the dashboard, where a dashboard request is ignored as
    redundant (and doesn't open the dashboard), so it goes to theater first."""
    vrcmd("--dock-overlay", "theater", key)
    time.sleep(0.5)
    vrcmd("--dock-overlay", "dashboard", key)
    time.sleep(0.8)
    vrcmd("--dock-overlay", "world", key)
    time.sleep(0.8)
    vrcmd("--hidedashboard")
    time.sleep(0.8)


def apply_gamescope(wait=0):
    layout = load_layout()
    count = screen_count(layout)
    deadline = time.time() + wait
    keys = screen_keys()
    while len(keys) < count and time.time() < deadline:
        time.sleep(1)
        keys = screen_keys()
    if wait:
        time.sleep(3)  # let Plasma draw before the screens start moving
    if not keys:
        raise RuntimeError("no Frametop screens in SteamVR; is the desktop running?")
    keys = keys[:count]
    helper = Socket(HELPER, "the pointer helper (frametop-pointer.service)")
    f = helper.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    vrcmd("--hidedashboard")
    time.sleep(0.5)
    shown = visible_keys()
    size = None
    for key in keys:
        if key not in shown:
            float_screen(key)
        try:
            m = helper_measure(helper, key)
        except RuntimeError:
            float_screen(key)  # floating but without a position: float it again
            m = helper_measure(helper, key)
        # The landscape size: a screen's panel is always landscape before it's rolled.
        size = size or (tuple(sorted(m["size"], reverse=True)))
    results = []
    for key, t in zip(keys, plan(layout, len(keys), size)):
        center = tuple(e + v for e, v in zip(eye, turn_yaw(t["pos"], heading)))
        reply = helper.ask("place %s %.4f %.4f %.4f %.3f %.3f %.3f" % (key, *center, t["face"][0] + heading,
                                                                        t["face"][1], t["roll"]), timeout=30)
        log(reply)
        results.append(reply)
    if size and list(size) != layout.get("panel_size"):
        layout["panel_size"] = [round(size[0], 4), round(size[1], 4)]
        save_layout(layout)
    return results


def capture_gamescope():
    layout = load_layout()
    helper = Socket(HELPER, "the pointer helper (frametop-pointer.service)")
    f = helper.ask("head").split()
    eye, heading = tuple(map(float, f[1:4])), float(f[4])
    shown = visible_keys()
    keys = [k for k in screen_keys() if k in shown]
    if not keys:
        raise RuntimeError("no floating screens to capture (screens docked in the dashboard don't count)")
    screens = []
    for i, key in enumerate(keys):
        m = helper_measure(helper, key)
        entry = dict(screen_entry(layout, i))
        entry.update(relative_pose(m["center"], m["x"], m["z"], eye, heading))
        screens.append(entry)
        layout["panel_size"] = [round(v, 4) for v in sorted(m["size"], reverse=True)]
    layout["screens"] = screens + layout.get("screens", [])[len(screens):]
    layout["mode"] = "custom"
    save_layout(layout)
    return screens



# ---------------------------------------------------------------- remote displays (docs/remote-displays.md)

def remote_displays(layout):
    """[(number, host, display)]: each display's screen number (its "screen", kept so it
    doesn't move when another is removed; or the first free one)."""
    entries = [(h, d) for h in layout.get("hosts", []) for d in h.get("displays", [])]
    used, out = set(), []
    for h, d in entries:
        n = d.get("screen")
        if isinstance(n, int) and REMOTE_FIRST <= n < REMOTE_FIRST + REMOTE_MAX and n not in used:
            used.add(n)
            d["_kept"] = True
    for h, d in entries:
        if not d.pop("_kept", False):
            free = [k for k in range(REMOTE_FIRST, REMOTE_FIRST + REMOTE_MAX) if k not in used]
            if not free:
                continue
            d["screen"] = free[0]
            used.add(free[0])
        out.append((d["screen"], h, d))
    return out


def find_display(layout, display_id):
    for n, host, d in remote_displays(layout):
        if d.get("id") == display_id:
            return n, host, d
    raise RuntimeError(f"no remote display {display_id!r}")


def remote_size(d):
    """A remote display's size in VR (width, height) in metres."""
    w, h = d.get("size", [2560, 1440])
    m = float(d.get("metres", max(1.0, w / REMOTE_PIXELS_PER_METRE)))
    return m, m * h / w


def plan_remotes(layout, count, displays):
    """The remote displays' poses in the head frame (as plan()): their own place, or else a
    row above the screens, hinged like the arc preset."""
    out = [None] * len(displays)
    for k, (_, _, d) in enumerate(displays):
        if "pos" in d:
            out[k] = {"pos": tuple(d["pos"]), "face": tuple(d.get("face", yaw_pitch(d["pos"]))),
                      "roll": float(d.get("roll", 0))}
    free = [k for k in range(len(displays)) if out[k] is None and not displays[k][2].get("off")]
    if not free:
        return out
    p = layout["preset"]
    dist = max(0.3, float(p.get("distance", 2.0)))
    gap = max(0.0, float(p.get("gap", 0.05)))
    span = lambda m, at: 2 * math.degrees(math.atan(m / 2 / at))
    top = 0.0
    for i, t in enumerate(plan(layout, count)):
        x, y, z = t["pos"]
        at = max(0.3, math.sqrt(x * x + y * y + z * z))
        top = max(top, math.degrees(math.atan2(y, math.hypot(x, z))) + span(screen_size(layout, i)[1], at) / 2)
    sizes = [remote_size(displays[k][2]) for k in free]
    pitch = top + span(gap, dist) + max(span(h, dist) for _, h in sizes) / 2
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    for k, (x, z, yaw) in zip(free, _chain([w for w, _ in sizes], dist, gap)):
        out[k] = {"pos": (x * cp, math.hypot(x, z) * sp, z * cp), "face": (yaw, pitch), "roll": 0.0}
    return out


def remotes_running(sock):
    """ft-screens' remote screens, {number: state}; None from an ft-screens without them."""
    try:
        fields = sock.ask("remotes").split()[2:]
    except RuntimeError:
        return None
    return {int(e.split(":")[0]): e.split(":")[2] for e in fields}


def start_remote(sock, n, host, d):
    """Its stream (ft-screens starts it over only if the settings changed). ft-screens'
    reply: "ok restored" when its panel is back where it was before a disconnect."""
    w, h = d.get("size", [2560, 1440])
    label = f"{host.get('name') or host['address']}: {d.get('label') or d['id']}"
    return sock.ask(f"remote {n} start {d['client']} {host['address']} {d['app']} {int(w)}x{int(h)} "
                    f"{int(d.get('fps', 60))} {int(d.get('bitrate', 0))} {remote_size(d)[0]:.3f} {label}")


def place_remote(sock, n, d, t, eye, heading):
    center = tuple(e + v for e, v in zip(eye, turn_yaw(t["pos"], heading)))
    sock.ask(f"width {n} {remote_size(d)[0]:.4f}")
    sock.ask(f"curve {n} {float(d.get('curve', 0)):.3f}")
    sock.ask("place %d %.4f %.4f %.4f %.3f %.3f %.3f" % (n, *center, t["face"][0] + heading, t["face"][1], t["roll"]))
    pin = d.get("pin")
    if pin and len(pin.get("rel", [])) == 12:
        try:
            sock.ask(f"pin {n} {pin['hand']} " + " ".join(f"{v:.5f}" for v in pin["rel"]))
        except RuntimeError as e:
            log(f"remote display {d['id']}: {e}")  # that controller isn't on
    sock.ask(f"{'conceal' if d.get('hidden') else 'reveal'} {n}")


def host_answers(host, timeout=1.5):
    """Whether a host's Vibepollo answers, at its address or its Steam Link dongle's (as its
    route allows)."""
    route = host.get("route", "auto")
    addresses = ([] if route == "dongle" else [host.get("address")]) + \
        ([] if route == "network" else list(host.get("direct", [])))
    for address in filter(None, addresses):
        try:
            with socket.create_connection((address, WEB_UI_PORT), timeout=timeout):
                return True
        except OSError:
            pass
    return False


def profile_remotes(layout, name):
    """Remote displays the profile has a place for go there; the others stay where they are."""
    remote = layout.get("profiles", {}).get(name, {}).get("remote", {})
    for _, _, d in remote_displays(layout):
        if d.get("id") in remote:
            for k in SPATIAL:
                d.pop(k, None)
            d.update(json.loads(json.dumps({k: v for k, v in remote[d["id"]].items() if k in SPATIAL})))


def open_remotes(layout, name):
    """Connect the remote displays profile `name` was saved with (in the layout: apply starts
    their streams), each whose host answers now. A host that doesn't is skipped, and its
    displays stay disconnected."""
    remote = layout.get("profiles", {}).get(name, {}).get("remote", {})
    wanted = [(h, d) for _, h, d in remote_displays(layout) if d.get("id") in remote and d.get("off")]
    hosts = list({id(h): h for h, _ in wanted}.values())
    if not hosts:
        return
    with concurrent.futures.ThreadPoolExecutor(len(hosts)) as pool:
        up = dict(zip(map(id, hosts), pool.map(host_answers, hosts)))
    for h, d in wanted:
        if up[id(h)]:
            d.pop("off", None)
            log(f"remote display {d['id']}: connecting (profile {name!r})")
        else:
            log(f"remote display {d['id']}: {h.get('name') or h['address']} isn't answering; skipped")


def start_remotes(sock, layout, only=None):
    """Start the remote displays' streams (or just the ids in `only`; ft-screens leaves one
    running with the same settings alone), and stop the streams of ones no longer listed or
    disconnected ("off"). They start without a head pose too: placing them waits for one
    (place_remotes)."""
    displays = remote_displays(layout)
    running = remotes_running(sock)
    if running is None:
        if displays:
            log("remote displays: this ft-screens can't show them (build it again)")
        return False
    if only is None:
        for n in set(running) - {n for n, _, d in displays if not d.get("off")}:
            sock.ask(f"remote {n} stop")
    for n, host, d in displays:
        if d.get("off"):
            continue
        if only is None or d.get("id") in only:
            try:
                start_remote(sock, n, host, d)
            except RuntimeError as e:
                log(f"remote display {d.get('id')}: {e}")
    return True


def place_remotes(sock, layout, count, eye, heading, only=None):
    displays = remote_displays(layout)
    for (n, host, d), t in zip(displays, plan_remotes(layout, count, displays)):
        if d.get("off"):
            continue  # disconnected: no panel
        if only is None or d.get("id") in only:
            try:
                place_remote(sock, n, d, t, eye, heading)
            except RuntimeError as e:
                log(f"remote display {d.get('id')}: {e}")
    on = [d for _, _, d in displays if not d.get("off") and (only is None or d.get("id") in only)]
    if on:
        log(f"arranged {len(on)} remote display(s)")


def capture_remotes(sock, layout, eye, heading):
    """Where the remote displays are now (each one running) into their entries."""
    for n, _, d in remote_displays(layout):
        try:
            g = parse_get(sock.ask(f"get {n}"))
        except RuntimeError:
            continue  # not running
        d.update(relative_pose(g["center"], g["x"], g["z"], eye, heading))
        d["metres"] = round(g["metres"], 4)
        d["curve"] = round(g["curve"], 3)
        d.pop("pin", None)
        if "rel" in g:
            d["pin"] = {"hand": g["hand"], "rel": g["rel"]}


def in_container():
    return os.path.exists("/run/.containerenv")


def run_stream(*args, timeout=60):
    """ft-stream (it runs in the dev container, as ft-screens does)."""
    cmd = [STREAM, *args]
    if not in_container():
        cmd = [os.path.expanduser("~/.local/bin/distrobox"), "enter", "dev", "--", *cmd]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout, r.stderr


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-") or "display"


def remote_anchor(sock, layout):
    """(eye, heading) to place remote displays from: the head, or with no head pose (the
    headset off), where the last arrangement was made from, worked out from where screen 1
    is and where the layout puts it."""
    try:
        f = sock.ask("head").split()
        return tuple(map(float, f[1:4])), float(f[4])
    except RuntimeError:
        g = parse_get(sock.ask("get 1"))
        if g["hand"] != "none":
            raise RuntimeError("no head pose, and screen 1 is pinned")
        t = plan(layout, screen_count(layout))[0]
        heading = yaw_pitch(tuple(-c for c in g["z"]))[0] - t["face"][0]
        return tuple(c - v for c, v in zip(g["center"], turn_yaw(t["pos"], heading))), heading


def start_remotes_now(layout, only):
    """Start and place remote displays now, if the desktop runs."""
    try:
        sock = screens_socket()
        if not start_remotes(sock, layout, only):
            return
        eye, heading = remote_anchor(sock, layout)
        place_remotes(sock, layout, screen_count(layout), eye, heading, only=only)
    except RuntimeError as e:
        log(f"not placed now: {e}")


def remote_command(args):
    """ft-layout remote list|monitors|add|set|connect|disconnect|remove (see the usage)."""
    what = args[0] if args else ""
    if what == "list" and len(args) == 1:
        layout = load_layout()
        try:
            running = remotes_running(screens_socket()) or {}
        except RuntimeError:
            running = {}
        for n, host, d in remote_displays(layout):
            w, h = d.get("size", [2560, 1440])
            state = "disconnected" if d.get("off") else running.get(n, "off")
            state += f" route {host.get('route', 'auto')}"
            print(f"{n} {d['id']} {host.get('name')} {host['address']} {d['app']} {w}x{h}@{d.get('fps', 60)} "
                  f"{state}{' hidden' if d.get('hidden') else ''}  {d.get('label', '')}")
        return
    if what == "host" and len(args) >= 3:
        layout = load_layout()
        host = next((h for h in layout.get("hosts", []) if h.get("name") == args[1]), None)
        if host is None:
            raise RuntimeError(f"no host called {args[1]!r}")
        for kv in args[2:]:
            k, _, v = kv.partition("=")
            if k == "route" and v in ("auto", "network", "dongle"):
                host["route"] = v
            elif k == "direct" and (v == "none" or re.fullmatch(r"[A-Za-z0-9.:-]+(,[A-Za-z0-9.:-]+)*", v)):
                host["direct"] = [] if v == "none" else v.split(",")
            else:
                raise RuntimeError(f"remote host: {kv!r}? (route=auto|network|dongle direct=ADDRESS[,ADDRESS]|none)")
        save_layout(layout)
        # Its streams start over the new way, their panels where they are (ft-screens keeps
        # their places across a stop in the same run).
        try:
            sock = screens_socket()
            running = remotes_running(sock) or {}
            for n, h, d in remote_displays(layout):
                if h is host and n in running and not d.get("off"):
                    sock.ask(f"remote {n} stop")
                    start_remote(sock, n, h, d)
                    log(f"{d['id']}: starting over")
        except RuntimeError as e:
            log(f"saved; not applied now: {e}")
        return
    if what in ("connect", "disconnect") and len(args) >= 2:
        layout = load_layout()
        found = [find_display(layout, i) for i in args[1:]]
        for _, _, d in found:
            if what == "connect":
                d.pop("off", None)
            else:
                d["off"] = True
        save_layout(layout)
        if what == "connect":
            # Back where it was if ft-screens still knows (this run); else placed like the
            # others, from where you look now.
            try:
                sock = screens_socket()
                unplaced = {d["id"] for n, host, d in found if start_remote(sock, n, host, d) != "ok restored"}
            except RuntimeError as e:
                log(f"saved; not started now: {e}")
                return
            for _, _, d in found:
                log(f"connected {d['id']}" + ("" if d["id"] in unplaced else " (where it was)"))
            if unplaced:
                try:
                    eye, heading = remote_anchor(sock, layout)
                    place_remotes(sock, layout, screen_count(layout), eye, heading, only=unplaced)
                except RuntimeError as e:
                    log(f"not placed now: {e}")
            return
        try:
            sock = screens_socket()
            for n, _, d in found:
                sock.ask(f"remote {n} stop")
                log(f"disconnected {d['id']}")
        except RuntimeError as e:
            log(f"saved; not stopped now: {e}")
        return
    if what == "monitors" and len(args) == 2:
        code, out, err = run_stream("monitors", args[1])
        if code:
            raise RuntimeError(err.strip() or out.strip() or "ft-stream monitors failed")
        print(out.strip())
        return
    if what == "add" and len(args) >= 4:
        host_name, address, app = args[1], args[2], args[3]
        opts = dict(zip(args[4::2], args[5::2]))
        unknown = set(opts) - {"--size", "--fps", "--bitrate", "--label", "--client"}
        if unknown or len(args[4:]) % 2:
            raise RuntimeError("remote add: options are --size WxH --fps F --bitrate KBPS --label TEXT --client NAME")
        if not re.fullmatch(r"[A-Za-z0-9.:-]+", address) or not re.fullmatch(r"[A-Za-z0-9{}._:-]+", app):
            raise RuntimeError("remote add: the address or app has other characters")
        layout = load_layout()
        displays = remote_displays(layout)
        if len(displays) >= REMOTE_MAX:
            raise RuntimeError(f"at most {REMOTE_MAX} remote displays")
        label = opts.get("--label") or ("Virtual" if app == "monitor" else app.split(":", 1)[-1])
        ids = {d["id"] for _, _, d in displays}
        display_id = base = slug(f"{host_name}-{label}")
        for k in range(2, 100):
            if display_id not in ids:
                break
            display_id = f"{base}-{k}"
        client = opts.get("--client") or f"frametop-{display_id}"
        if not re.fullmatch(r"[A-Za-z0-9._-]+", client):
            raise RuntimeError("remote add: the client name has other characters")
        size = [2560, 1440]
        if "--size" in opts:
            size = [int(v) for v in opts["--size"].lower().split("x")]
        elif app.startswith("display:"):  # the monitor's own resolution
            code, out, _ = run_stream("monitors", address)
            try:
                for m in (json.loads(out[out.index("["):]) if code == 0 else []):
                    if m.get("device_id") == app[8:]:
                        res = m["info"]["resolution"]
                        size = [int(res["width"]), int(res["height"])]
            except (ValueError, KeyError, TypeError):
                pass
        code, out, err = run_stream("pair", address, "--id", client, timeout=30)
        if code:
            raise RuntimeError(f"pairing {client} with {address} failed: {(err or out).strip()}")
        log(out.strip().splitlines()[-1] if out.strip() else f"paired {client}")
        host = next((h for h in layout.setdefault("hosts", []) if h.get("name") == host_name), None)
        if host is None:
            host = {"name": host_name, "address": address, "displays": []}
            layout["hosts"].append(host)
        host["address"] = address
        host["displays"].append({"id": display_id, "client": client, "app": app, "label": label, "size": size,
                                 "fps": int(opts.get("--fps", 60)), "bitrate": int(opts.get("--bitrate", 0)),
                                 "metres": round(max(1.0, size[0] / REMOTE_PIXELS_PER_METRE), 3)})
        remote_displays(layout)  # gives it a screen number
        save_layout(layout)
        print(display_id)
        start_remotes_now(layout, {display_id})
        return
    if what == "set" and len(args) >= 3:
        layout = load_layout()
        n, host, d = find_display(layout, args[1])
        for kv in args[2:]:
            k, _, v = kv.partition("=")
            if k == "size" and re.fullmatch(r"\d+x\d+", v.lower()):
                d["size"] = [int(x) for x in v.lower().split("x")]
            elif k in ("fps", "bitrate") and v.isdigit():
                d[k] = int(v)
            elif k == "label" and v:
                d["label"] = v
            elif k == "metres" and re.fullmatch(r"\d+(\.\d+)?", v) and 0.15 <= float(v) <= 12:
                d["metres"] = float(v)
            elif k == "app" and re.fullmatch(r"[A-Za-z0-9{}._:-]+", v):
                d["app"] = v
            else:
                raise RuntimeError(f"remote set: {kv!r}? (size=WxH fps=F bitrate=KBPS label=TEXT app=APP metres=M)")
        save_layout(layout)
        if d.get("off"):
            return  # disconnected: the new settings are for its next connection
        try:
            start_remote(screens_socket(), n, host, d)  # starts over with the new settings
        except RuntimeError as e:
            log(f"saved; not applied now: {e}")
        return
    if what == "remove" and len(args) == 2:
        layout = load_layout()
        n, host, d = find_display(layout, args[1])
        try:
            screens_socket().ask(f"remote {n} stop")
            time.sleep(2)  # its stream releases a Remote Monitor on the way out
        except RuntimeError:
            pass
        host["displays"].remove(d)  # the host stays (Remote Displays removes hosts)
        for profile in layout.get("profiles", {}).values():
            profile.get("remote", {}).pop(d["id"], None)
        save_layout(layout)
        code, out, err = run_stream("unpair", host["address"], "--id", d["client"], timeout=30)
        said = (out or err).strip().splitlines()
        log(f"removed {d['id']}; {said[-1] if said else f'unpair exited {code}'}")
        return
    raise RuntimeError("remote list | monitors ADDRESS | add HOST ADDRESS APP [options] | set ID KEY=VALUE... | "
                       "host NAME route=...|direct=... | connect ID... | disconnect ID... | remove ID")


def apply(wait=0):
    return apply_screens(wait) if backend() == "screens" else apply_gamescope(wait)


def capture():
    """Where the screens are now as the custom layout. It's no longer a named one's until
    `save NAME` (placed by hand since)."""
    screens = capture_screens() if backend() == "screens" else capture_gamescope()
    layout = load_layout()
    if layout.pop("active", None) is not None:
        save_layout(layout)
    return screens


# ---------------------------------------------------------------- named layouts

def layout_names(layout):
    return sorted(layout.get("layouts", {}), key=str.casefold)


def check_name(name):
    name = " ".join(name.split())
    if not name or len(name) > 40:
        raise RuntimeError("a layout's name needs 1 to 40 characters")
    return name


def save_named(layout, name):
    """The custom arrangement (as captured) under `name`, replacing one of that name, and
    in use."""
    name = check_name(name)
    screens = [s for s in layout.get("screens", [])[:screen_count(layout)] if "pos" in s]
    if not screens:
        raise RuntimeError("nothing to save: no arrangement captured")
    layout.setdefault("layouts", {})[name] = [{k: s[k] for k in SPATIAL if k in s} for s in screens]
    # The remote displays connected now, like the apps open now.
    remote = {d["id"]: {k: d[k] for k in SPATIAL if k in d} for _, _, d in remote_displays(layout)
              if not d.get("off") and "pos" in d}
    profile = layout.setdefault("profiles", {}).setdefault(name, {})
    if remote:
        profile["remote"] = remote
    else:
        profile.pop("remote", None)
    layout["mode"], layout["active"] = "custom", name
    return name


def use_named(layout, name):
    """Make a named layout the custom arrangement (not arranged yet). A layout saved with
    fewer screens leaves the others where the preset would put them, or where they were
    saved last; one saved with more keeps its extra screens for later."""
    saved = layout.get("layouts", {}).get(name)
    if saved is None:
        raise RuntimeError(f"no layout called {name!r}")
    count = screen_count(layout)
    preset = plan(dict(layout, mode="preset"), count)
    screens = layout.setdefault("screens", [])
    while len(screens) < count:
        screens.append({})
    for i in range(count):
        if i < len(saved):
            for k in SPATIAL:
                screens[i].pop(k, None)
            screens[i].update(json.loads(json.dumps(saved[i])))
        elif "pos" not in screens[i]:
            screens[i].update({"pos": list(preset[i]["pos"]), "face": list(preset[i]["face"]),
                               "roll": preset[i]["roll"]})
    profile_remotes(layout, name)
    layout["mode"], layout["active"] = "custom", name


def rename_named(layout, old, new):
    new = check_name(new)
    named = layout.get("layouts", {})
    if old not in named:
        raise RuntimeError(f"no layout called {old!r}")
    if new != old and new in named:
        raise RuntimeError(f"there's already a layout called {new!r}")
    named[new] = named.pop(old)
    profiles = layout.get("profiles", {})
    if old in profiles:
        profiles[new] = profiles.pop(old)
    if layout.get("default_profile") == old:
        layout["default_profile"] = new
    if layout.get("active") == old:
        layout["active"] = new
    return new


def delete_named(layout, name):
    """The screens stay where the layout put them, as an unnamed custom arrangement."""
    if layout.get("layouts", {}).pop(name, None) is None:
        raise RuntimeError(f"no layout called {name!r}")
    layout.get("profiles", {}).pop(name, None)
    if layout.get("default_profile") == name:
        layout.pop("default_profile")
    if layout.get("active") == name:
        layout.pop("active")


# ---------------------------------------------------------------- profiles (docs/profiles.md)

def ask_float(text, timeout=6.0):
    """ft-floatd (floating windows, in the desktop's session); None if it isn't running."""
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
    sock.bind("")
    sock.settimeout(timeout)
    try:
        sock.sendto(text.encode(), FLOAT)
        return sock.recv(1 << 20).decode()
    except OSError:
        return None
    finally:
        sock.close()


def capture_profile(layout, name):
    """The desktop's apps and hidden screens into profile `name` (kept as they were if
    ft-floatd doesn't answer)."""
    hidden = [i + 1 for i in range(screen_count(layout)) if screen_entry(layout, i).get("hidden")]
    profile = layout.setdefault("profiles", {}).setdefault(name, {})
    profile["hidden"] = hidden
    remote = profile.get("remote", {})  # the ones connected (save_named)
    for _, _, d in remote_displays(layout):
        if d["id"] in remote:
            remote[d["id"]]["hidden"] = bool(d.get("hidden"))
    reply = ask_float("windows")
    if reply and reply.startswith("ok "):
        profile["windows"] = json.loads(reply[3:])
        log(f"profile {name!r}: {len(profile['windows'])} windows, hidden screens {hidden or 'none'}")
    else:
        log(f"profile {name!r}: the apps weren't saved ({reply or 'ft-floatd is not running'})")


def use_hidden(layout, name):
    """A profile's hidden screens as the screens' own setting (applied with the arrangement)."""
    profile = layout.get("profiles", {}).get(name)
    if profile is None:
        return
    hidden = set(profile.get("hidden", []))
    screens = layout.setdefault("screens", [])
    for i in range(screen_count(layout)):
        while len(screens) <= i:
            screens.append({})
        if i + 1 in hidden:
            screens[i]["hidden"] = True
        else:
            screens[i].pop("hidden", None)
    remote = profile.get("remote", {})
    for _, _, d in remote_displays(layout):
        if "hidden" in remote.get(d.get("id"), {}):
            if remote[d["id"]]["hidden"]:
                d["hidden"] = True
            else:
                d.pop("hidden", None)


def open_apps(name, wait=0):
    """Have ft-floatd open a profile's apps (waiting up to `wait` seconds for it to start)."""
    if not load_layout().get("profiles", {}).get(name, {}).get("windows"):
        return
    deadline = time.time() + wait
    while True:
        reply = ask_float(f"profile {name}")
        if reply is not None or time.time() >= deadline:
            break
        time.sleep(1)
    log(f"apps: {reply or 'ft-floatd is not running'}")


def launcher_name(name):
    slug = re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") or "profile"
    return f"frametop-profile-{slug}.desktop"


def write_launchers(layout):
    """A launcher entry for each profile (SteamVR's Launch a program list, the Application
    Launcher, KRunner), and none for ones that are gone."""
    os.makedirs(LAUNCHERS, exist_ok=True)
    want = {}
    for name in layout_names(layout):
        # Quoted for Exec (\" \` \$ \\), then each backslash doubled for the key file.
        quoted = ('"' + re.sub(r'(["`$\\])', r"\\\1", name) + '"').replace("\\", "\\\\")
        want[launcher_name(name)] = "\n".join([
            "[Desktop Entry]", "Type=Application", f"Name=Frametop: {name}",
            "Comment=Open the Frametop desktop in this profile: its screens and apps",
            f"Exec={os.path.join(REPO, 'layout', 'ft-layout')} open {quoted}",
            "Icon=preferences-desktop-display", "Categories=Utility;", "X-Frametop-Profile=true", ""])
    for f in os.listdir(LAUNCHERS):
        if f.startswith("frametop-profile-") and f.endswith(".desktop") and f not in want:
            os.remove(os.path.join(LAUNCHERS, f))
    for f, text in want.items():
        path = os.path.join(LAUNCHERS, f)
        try:
            with open(path) as old:
                if old.read() == text:
                    continue
        except OSError:
            pass
        with open(path, "w") as out:
            out.write(text)


def start_profile(layout):
    """The profile the desktop starts with: FT_PROFILE, else default_profile."""
    name = os.environ.get("FT_PROFILE") or layout.get("default_profile")
    return name if name and name in layout.get("layouts", {}) else None


def desktop_running():
    try:
        screens_socket().ask("screens", timeout=2)
        return True
    except RuntimeError:
        return False


def start_desktop(profile):
    """Start the Frametop desktop in a profile (as desktops.sh start does)."""
    if subprocess.run(["pgrep", "-x", "vrcompositor"], capture_output=True).returncode != 0:
        raise RuntimeError("SteamVR isn't running")
    subprocess.run(["systemctl", "--user", "reset-failed", "frametop-desktop"], capture_output=True)
    session = os.path.join(REPO, "session", "frametop-session.sh")
    r = subprocess.run(["systemd-run", "--user", "--collect", "--quiet", "--unit", "frametop-desktop",
                        f"--setenv=FT_PROFILE={profile}", "bash", "-c",
                        f'exec "{session}" > /tmp/frametop-session.log 2>&1'], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"couldn't start the desktop: {r.stderr.strip()}")
    log(f"starting the desktop in {profile!r}")


# ---------------------------------------------------------------- KWin (scale, positions, primary)

def nested_env():
    """Environment of the running Frametop Plasma session (its private bus and runtime dir)."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open(f"/proc/{pid}/comm") as f:
                if f.read().strip() != "plasmashell":
                    continue
            with open(f"/proc/{pid}/environ", "rb") as f:
                env = dict(e.split("=", 1) for e in f.read().decode(errors="replace").split("\0") if "=" in e)
        except OSError:
            continue
        if env.get("XDG_RUNTIME_DIR", "").endswith("/frametop"):
            keep = ("DBUS_SESSION_BUS_ADDRESS", "WAYLAND_DISPLAY", "XDG_RUNTIME_DIR", "XDG_CONFIG_HOME")
            return dict(os.environ, **{k: env[k] for k in keep if k in env})
    return None


def outputs(env):
    """KWin's outputs for the screens, in screen order (WL-0, WL-1, ...)."""
    try:
        data = json.loads(subprocess.run(["kscreen-doctor", "-j"], capture_output=True, text=True, env=env,
                                         timeout=10).stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return []
    outs = [o for o in data.get("outputs", []) if o.get("connected")]
    if backend() == "screens":
        # Outputs after the screens are spares for floating windows (ft-floatd places them).
        count = screen_count()
        outs = [o for o in outs if not re.fullmatch(r"WL-(\d+)", o.get("name", "")) or
                int(o["name"][3:]) < count]
    return sorted(outs, key=lambda o: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", o.get("name", ""))])


KSCREEN_ROTATION = {1: "normal", 2: "left", 4: "inverted", 8: "right"}  # kscreen-doctor -j "rotation"


def arrangement(count):
    """The screens as you see them from where you are: columns left to right, each top to
    bottom (0-based screen indices), for KWin's output positions. Screens pinned to a
    wrist come last. None when there's nothing to go by (gamescope, no ft-screens)."""
    if backend() != "screens":
        return None
    try:
        sock = screens_socket()
        f = sock.ask("head").split()
        eye, heading = tuple(map(float, f[1:4])), float(f[4])
        gets = [parse_get(sock.ask(f"get {i + 1}")) for i in range(count)]
    except (RuntimeError, ValueError, IndexError):
        return None
    seen, pinned = [], []
    for i, g in enumerate(gets):
        if g["hand"] != "none":
            pinned.append(i)
            continue
        rel = turn_yaw(tuple(c - e for c, e in zip(g["center"], eye)), -heading)
        yaw, pitch = yaw_pitch(rel)
        half = math.degrees(math.atan2(g["metres"] / 2, max(0.1, math.sqrt(dot(rel, rel)))))
        seen.append({"i": i, "x": -yaw, "pitch": pitch, "half": half})  # x grows to the right
    seen.sort(key=lambda b: b["x"])
    columns = []
    for b in seen:
        # One above the other: centres closer sideways than half the narrower screen.
        if columns and abs(b["x"] - columns[-1][-1]["x"]) < min(b["half"], columns[-1][-1]["half"]):
            columns[-1].append(b)
        else:
            columns.append([b])
    return [[b["i"] for b in sorted(c, key=lambda b: -b["pitch"])] for c in columns] + [[i] for i in pinned]


def send_scales(outs):
    """KWin's scale for each screen, as it is now, to ft-screens: KWin's nested backend
    doesn't undo its scale on pointer input, so ft-screens does (panel pixels / scale)."""
    if backend() != "screens":
        return
    try:
        sock = screens_socket()
        for i, o in enumerate(outs):
            reply = sock.ask(f"scale {i + 1} {float(o.get('scale', 1)):g}")
            if not reply.startswith("ok"):
                log(f"screen {i + 1}: scale: {reply}")
    except RuntimeError as e:
        log(f"scale: {e}")


def logical_size(o):
    """An output's size in logical units. kscreen's "size" is in pixels (already turned for a
    rotation); positions are logical, the pixels divided by the scale (KWin rounds up)."""
    s = float(o.get("scale", 1))
    return (math.ceil(o["size"]["width"] / s - 1e-6), math.ceil(o["size"]["height"] / s - 1e-6))


def apply_scales():
    """Per-screen scale and rotation, positions side by side, and the primary screen (the
    taskbar goes there) to KWin, which keeps them in the session's config."""
    env = nested_env()
    if not env:
        raise RuntimeError("the Frametop desktop isn't running")
    layout = load_layout()
    args = []
    outs = outputs(env)
    for i, o in enumerate(outs):
        s = screen_scale(layout, i)
        if abs(float(o.get("scale", 1)) - s) > 1e-3:
            args.append(f"output.{o['id']}.scale.{s:g}")
        rot = screen_rotation(layout, i)
        if KSCREEN_ROTATION.get(o.get("rotation"), "normal") != rot:
            args.append(f"output.{o['id']}.rotation.{rot}")
    if outs:
        p = outs[min(primary_screen(layout), len(outs) - 1)]
        if p.get("priority") != 1:
            args.append(f"output.{p['id']}.priority.1")
    if args:
        subprocess.run(["kscreen-doctor", *args], capture_output=True, env=env, timeout=20)
    # Laid out as you see the screens around you (arrangement), centred on one line, so
    # the pointer and dragged windows cross to the screen you see next to this one.
    outs = outputs(env)
    send_scales(outs)
    sizes = [logical_size(o) for o in outs if o.get("size")]
    if len(sizes) == len(outs) and outs:
        columns = arrangement(len(outs))
        if not columns or sorted(i for c in columns for i in c) != list(range(len(outs))):
            # Nothing to go by (no head pose with the headset off, say): keep KWin's order.
            columns = [[i] for i in sorted(range(len(outs)), key=lambda i: (outs[i].get("pos", {}).get("x", 0),
                                                                            outs[i].get("pos", {}).get("y", 0)))]
        widths = [max(sizes[i][0] for i in c) for c in columns]
        heights = [sum(sizes[i][1] for i in c) for c in columns]
        tallest, x, moves = max(heights), 0, []
        for c, cw, ch in zip(columns, widths, heights):
            y = (tallest - ch) // 2
            for i in c:
                want = (x + (cw - sizes[i][0]) // 2, y)
                y += sizes[i][1]
                o = outs[i]
                if (o.get("pos", {}).get("x"), o.get("pos", {}).get("y")) != want:
                    moves.append(f"output.{o['id']}.position.{want[0]},{want[1]}")
            x += cw
        if moves:
            subprocess.run(["kscreen-doctor", *moves], capture_output=True, env=env, timeout=20)
            args += moves
    return args


def kwin_follow():
    """KWin's outputs after the screens moved, if the desktop is up."""
    try:
        changes = apply_scales()
        log("kwin: " + (" ".join(changes) if changes else "unchanged"))
    except RuntimeError as e:
        log(f"kwin: {e}")


def remote_view():
    """Where the primary screen sits in the workspace (all screens' bounding box), in
    logical units: "x y width height workspace_width workspace_height". The VNC bridge
    (session/vnc-bridge.sh) shows that part of krdp's workspace stream."""
    env = nested_env()
    if not env:
        raise RuntimeError("the Frametop desktop isn't running")
    outs = [o for o in outputs(env) if o.get("enabled", True) and o.get("size") and o.get("pos")]
    if not outs:
        raise RuntimeError("the Frametop desktop has no screens yet")
    rects = [(o["pos"]["x"], o["pos"]["y"], *logical_size(o)) for o in outs]
    left, top = min(r[0] for r in rects), min(r[1] for r in rects)
    right, bottom = max(r[0] + r[2] for r in rects), max(r[1] + r[3] for r in rects)
    p = next((i for i, o in enumerate(outs) if o.get("priority") == 1), 0)
    x, y, w, h = rects[p]
    return f"{x - left} {y - top} {w} {h} {right - left} {bottom - top}"


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__.split("Usage")[1].split("\n", 1)[1])
        return 0 if len(argv) >= 2 else 2
    cmd = argv[1]
    try:
        if cmd == "plan":
            layout = load_layout()
            print(json.dumps(plan(layout, screen_count(layout))))
        elif cmd == "screen-args":
            print(screen_args())
        elif cmd == "remote-view":
            print(remote_view())
        elif cmd == "toggle":
            log(screens_socket().ask("toggle"))
        elif cmd in ("hide", "show") and len(argv) == 3:
            set_hidden(argv[2], cmd == "hide")
        elif cmd == "hidden":
            layout = load_layout()
            print(" ".join(str(i + 1) for i in range(screen_count(layout)) if screen_entry(layout, i).get("hidden")))
        elif cmd == "layouts":
            layout = load_layout()
            for name in layout_names(layout):
                print(("* " if name == layout.get("active") and layout.get("mode") == "custom" else "  ") + name)
        elif cmd in ("rename", "delete") and len(argv) == (4 if cmd == "rename" else 3):
            layout = load_layout()
            if cmd == "rename":
                rename_named(layout, argv[2], argv[3])
            else:
                delete_named(layout, argv[2])
            save_layout(layout)
            write_launchers(layout)
        elif cmd == "launchers":
            write_launchers(load_layout())
        elif cmd == "default" and len(argv) == 3:
            layout = load_layout()
            if argv[2] == "none":
                layout.pop("default_profile", None)
            elif argv[2] in layout.get("layouts", {}):
                layout["default_profile"] = argv[2]
            else:
                raise RuntimeError(f"no profile called {argv[2]!r}")
            save_layout(layout)
        elif cmd == "open" and len(argv) == 3:
            if argv[2] not in load_layout().get("layouts", {}):
                raise RuntimeError(f"no profile called {argv[2]!r}")
            if desktop_running():
                return main([argv[0], "use", argv[2]])
            start_desktop(argv[2])
        elif cmd == "start":
            # Desktop start (the session script): the profile it starts with, or the arrangement.
            layout = load_layout()
            name = start_profile(layout)
            if not name:
                return main([argv[0], "apply"] + argv[2:])
            use_named(layout, name)
            use_hidden(layout, name)
            open_remotes(layout, name)
            save_layout(layout)
            log(f"starting in profile {name!r}")
            wait = float(argv[argv.index("--wait") + 1]) if "--wait" in argv else 60
            with open(LOCK_PATH, "w") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                apply(wait)
                for _ in range(30):  # Plasma may still be starting
                    try:
                        log("kwin: " + (" ".join(apply_scales()) or "unchanged"))
                        break
                    except RuntimeError as e:
                        last = e
                        time.sleep(1)
                else:
                    log(f"kwin: {last}")
            open_apps(name, wait=90)  # ft-floatd starts with Plasma
        elif cmd == "remote":
            remote_command(argv[2:])
        elif cmd in ("pin", "unpin") and len(argv) >= 3:
            log(screens_socket().ask(" ".join(argv[1:])))
            kwin_follow()  # pinned screens go last
        elif cmd in ("apply", "capture", "scale") or (cmd in ("save", "use") and len(argv) == 3):
            with open(LOCK_PATH, "w") as lock:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    log("another ft-layout is already running")
                    return 1
                if cmd == "apply":
                    wait = float(argv[argv.index("--wait") + 1]) if "--wait" in argv else 0
                    if wait and not load_layout().get("auto", True):
                        log("auto-arrange is off")
                        if backend() == "screens":  # the visibility settings apply anyway
                            try:
                                sock = screens_socket()
                                deadline = time.time() + wait
                                while screens_up(sock) < screen_count() and time.time() < deadline:
                                    time.sleep(1)
                                send_visibility(sock, load_layout())
                                send_hidden(sock, load_layout())
                                layout = load_layout()
                                if start_remotes(sock, layout):  # streams start, arranged or not
                                    save_layout(layout)
                            except RuntimeError as e:
                                log(f"visibility: {e}")
                    else:
                        layout = load_layout()
                        name = layout.get("active") if layout.get("mode") == "custom" else None
                        if name in layout.get("profiles", {}):
                            # Arranged as the profile in use: its remote displays too.
                            profile_remotes(layout, name)
                            open_remotes(layout, name)
                            save_layout(layout)
                        apply(wait)
                    if not wait:
                        kwin_follow()
                    if wait:
                        # KWin keeps these, but new screens or a changed layout need them once.
                        for _ in range(30):  # Plasma may still be starting
                            try:
                                log("kwin: " + (" ".join(apply_scales()) or "unchanged"))
                                break
                            except RuntimeError as e:
                                last = e
                                time.sleep(1)
                        else:
                            log(f"kwin: {last}")
                elif cmd == "capture":
                    for i, s in enumerate(capture()):
                        log(f"screen {i + 1}: {s}")
                    kwin_follow()
                elif cmd == "save":
                    check_name(argv[2])
                    capture()
                    layout = load_layout()
                    name = save_named(layout, argv[2])
                    capture_profile(layout, name)
                    save_layout(layout)
                    write_launchers(layout)
                    log(f"saved layout {name!r}")
                    kwin_follow()
                elif cmd == "use":
                    layout = load_layout()
                    use_named(layout, argv[2])
                    use_hidden(layout, argv[2])
                    open_remotes(layout, argv[2])
                    save_layout(layout)
                    log(f"using layout {argv[2]!r}")
                    try:
                        apply()
                    except RuntimeError as e:
                        log(f"not arranged now: {e}")
                        try:  # the screens stay put, but the profile's hidden ones still hide
                            send_hidden(screens_socket(), layout)
                        except RuntimeError:
                            pass
                    else:
                        kwin_follow()
                    open_apps(argv[2])  # floating windows go relative to the screens, wherever they are
                else:
                    changes = apply_scales()
                    log("kwin: " + (" ".join(changes) if changes else "unchanged"))
        else:
            print(f"unknown command: {cmd}", file=sys.stderr)
            return 2
    except RuntimeError as e:
        log(f"error: {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
