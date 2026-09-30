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
   "visibility": {"mode": "always",   ft-screens: always | dashboard (only with the SteamVR
                  "wrist_angle": 60,    dashboard open) | gesture (while you look at a controller)
                  "gesture_hand": "left", "gesture_angle": 20},   | toggle (hidden until shown);
                                      wrist_angle: a pinned screen shows while you see its front
                                      within this many degrees
   "screens": [{"size": [w, h], "metres": 3.6,        ft-screens: pixels, and width in VR
                "curve": 0,                           ft-screens: cylinder radius in metres, 0 = flat
                "pin": {"hand": "left", "rel": [12]},  ft-screens: riding on that controller
                "scale": 1.0,                         KWin output scale (1.0 = 100%)
                "pos": [x, y, z], "face": [yaw, pitch], "roll": 0,   custom layout
                "rotation": "normal" | "left" | "right"}, ...],      gamescope only
   "panel_size": [w, h]}              gamescope: last measured panel size
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
  ft-layout plan                     print the arrangement as JSON (no VR needed)
  ft-layout scale                    per-screen scale, positions (as the screens are around
                                     you), and primary to KWin
  ft-layout screen-args              ft-screens' --screen arguments for the session script
  ft-layout toggle                   hide or show all screens (ft-screens)
  ft-layout pin all|N left|right     pin screens to a wrist as they are; unpin all|N
"""
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
DEFAULT_PANEL = (1.18, 0.664)  # gamescope: a floating 16:9 dashboard panel, measured on the Frame
PIXELS_PER_METRE = 800         # ft-screens: a new screen's default size in VR (1920 px: 2.4 m)
# controllers: when controllers' lasers work the screens (always | outside_games | dashboard).
# in_games: during a VR game, "always" hides the screens unless the dashboard is open (hide),
# or leaves them up (visible).
VISIBILITY = {"mode": "always", "wrist_angle": 60, "gesture_hand": "left", "gesture_angle": 20,
              "controllers": "outside_games", "in_games": "hide"}
DEFAULTS = {"auto": True, "mode": "preset",
            "preset": {"kind": "arc", "rows": 1, "distance": 2.0, "gap": 0.05, "height": 0.0},
            "screens": [], "panel_size": list(DEFAULT_PANEL)}


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
    save_layout(layout)
    return screens


# ---------------------------------------------------------------- gamescope (dashboard panels)

def vrcmd(*args, timeout=10):
    env = dict(os.environ, LD_LIBRARY_PATH=os.path.dirname(VRCMD))
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


def apply(wait=0):
    return apply_screens(wait) if backend() == "screens" else apply_gamescope(wait)


def capture():
    return capture_screens() if backend() == "screens" else capture_gamescope()


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
    """KWin's outputs, in screen order (WL-0, WL-1, ...)."""
    try:
        data = json.loads(subprocess.run(["kscreen-doctor", "-j"], capture_output=True, text=True, env=env,
                                         timeout=10).stdout)
    except (OSError, ValueError, subprocess.TimeoutExpired):
        return []
    outs = [o for o in data.get("outputs", []) if o.get("connected")]
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
    # kscreen's "size" is in pixels (already turned for a rotation); positions are in
    # logical units, the pixels divided by the scale (KWin rounds up).
    sizes = [(math.ceil(o["size"]["width"] / float(o.get("scale", 1)) - 1e-6),
              math.ceil(o["size"]["height"] / float(o.get("scale", 1)) - 1e-6))
             for o in outs if o.get("size")]
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
        elif cmd == "toggle":
            log(screens_socket().ask("toggle"))
        elif cmd in ("pin", "unpin") and len(argv) >= 3:
            log(screens_socket().ask(" ".join(argv[1:])))
            kwin_follow()  # pinned screens go last
        elif cmd in ("apply", "capture", "scale"):
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
                            except RuntimeError as e:
                                log(f"visibility: {e}")
                    else:
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
