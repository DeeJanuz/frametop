#!/usr/bin/env python3
"""The hand recorder's session runner (DESIGN.md: "Processes during a session", "Files", "The
script", "Feedback while recording", "Controls").

It reads script.json, starts what the session needs (ft-camd and a tracking ft-hands, only if
they aren't running), and runs the sections in order. Each section is one take: a recording
ft-hands child process (--record-only, 10 sets a second) plus prompts.jsonl, take.json and the
panel's poses.jsonl. The headset panel (ft-handpanel) shows the prompts; the live hands file
gives feedback ("I can't see your left hand").

Plain Python, standard library only: ft_handrec.py imports it, and it runs from the command line
for testing (in the dev container):

  python3 hands/rec/session.py --dry-run --speed 20 --next-after 0.2   # no processes: prints the panel commands
  python3 hands/rec/session.py --ring /tmp/ring --base /tmp/hr        # ft-ringplay's frames, no headset needed

Before anything starts it runs the camera check (hands/camcheck.py) and won't start while the
upper cameras are off (--ignore-cameras overrides it). If the hand-size section's first step sees
no hands at all, it stops that step and asks: try again, or stop (DESIGN.md, "Camera check").

Step mode (the default) shows each step and waits for Next (Space in the window, n or Enter
here); a 3-2-1 countdown, recorded, then the hold. Only the countdowns and holds are recorded:
each is a part of the take's recording (sets-N.bin). --auto is the old timed flow.

All _ns times are CLOCK_MONOTONIC nanoseconds.
"""
import argparse
import glob
import hashlib
import json
import math
import mmap
import os
import random
import re
import select
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
HANDS = os.path.join(REPO, "hands")
sys.path.insert(0, HANDS)
import camcheck  # noqa: E402  (hands/camcheck.py: are all four mono cameras running?)
sys.path.insert(0, HERE)
import sides  # noqa: E402  (hands/rec/sides.py: which side camera is which)
FT_HANDS = os.path.join(HANDS, "build", "ft-hands")
FT_CAMD = os.path.join(HANDS, "build", "ft-camd")
PANEL_BIN = os.path.join(HERE, "build", "ft-handpanel")
SCRIPT_PATH = os.path.join(HERE, "script.json")
POSES_DIR = os.path.join(HERE, "poses")   # the pose pictures: poses.json and its PNGs
BASE_DIR = os.path.expanduser("~/.local/share/frametop/hands/contrib")
PANEL_SOCKET = "ft_handpanel"
CAMD_UNIT = "frametop-handrec-camd.service"
HANDS_UNIT = "frametop-handrec-hands.service"

RECORD_HZ = 10
SIDES_READ_S = 0.5       # how often the tracking ft-hands' side camera decision is read
TICK_S = 0.05            # the session loop's step (real time)
FRESH_S = 0.3            # the hands file counts as live if published this recently
LOST_S = 1.5             # an asked-for hand lost this long gets a note
CONTROLLER_LOST_S = 1.0  # a controller off 200 (Running_OK) this long gets a note
TOUCH_M = 0.03           # touch the dot: the index tip within this of the dot
MIN_FREE = 1.5e9         # stop the session before the disk fills
COUNTDOWN_S = 3          # step mode: the 3-2-1 before each step, recorded
FIRST_SET_S = 3.0        # step mode: how long the hold may wait for its recording's first set
RESUME_HINT = "Paused. Resume: P in the Hand recorder window"
READY_TEXT = "Ready? Press Space or click Next"
# With the headset's button: it leads when no mouse is connected (the window's Next can't be clicked).
READY_BUTTON = "Ready? Press the button on the right side of the headset"
READY_BUTTON_MOUSE = "Ready? Press Space, click Next, or press the headset button"
KEYS_STEP = "Hand recorder window:  Space next  \u00b7  P pause  \u00b7  R redo  \u00b7  S skip section  \u00b7  Esc stop"
KEYS_AUTO = "Hand recorder window:  P pause  \u00b7  R redo  \u00b7  S skip section  \u00b7  Esc stop"
KEYS_STEP_BUTTON = ("Headset button: next, pause  \u00b7  Window: Space next  \u00b7  P pause  \u00b7  R redo  "
                    "\u00b7  S skip  \u00b7  Esc stop")
KEYS_AUTO_BUTTON = "Headset button: pause  \u00b7  Window: P pause  \u00b7  R redo  \u00b7  S skip section  \u00b7  Esc stop"
# The early no-hands stop (DESIGN.md, "Camera check"): the first step of this section has both
# hands up. If the live tracker publishes through its hold and never sees a hand, the session
# stops that step and asks: try again, or stop. Only when the tracker published in at least
# HANDS_CHECK_PUBLISHED of at least HANDS_CHECK_MIN_READS reads (about 20 a second): without
# a tracker it can't tell.
HANDS_CHECK_SECTION = "hand-size"
HANDS_CHECK_MIN_READS = 10
HANDS_CHECK_PUBLISHED = 0.5
NO_HANDS_TITLE = "I can't see your hands"
NO_HANDS_TEXT = "The hand tracker didn't see either of your hands during that whole step."
NO_HANDS_RETRY = ("Try again: hold both hands up in front of you, about 40 cm away. To stop instead: "
                  "Esc or Stop in the Hand recorder window.")


def mono_ns():
    return time.clock_gettime_ns(time.CLOCK_MONOTONIC)


def clock_sample():
    """[CLOCK_MONOTONIC ns, CLOCK_MONOTONIC_RAW - CLOCK_MONOTONIC ns], as ft-hands' raw_minus_mono_ns().
    sets.bin's capture_ns is on the RAW clock and everything else on MONOTONIC; the two drift
    apart with NTP's corrections (0.8 s apart and ~10 ppm on 2026-10-03), so take.json keeps
    samples to turn capture_ns into MONOTONIC: capture_ns - offset, interpolated by time."""
    a = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    r = time.clock_gettime_ns(time.CLOCK_MONOTONIC_RAW)
    b = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    mid = (a + b) // 2
    return [mid, r - mid]


def run_dir():
    return "/run/user/%d/frametop-hands" % os.getuid()


def in_container():
    return os.path.exists("/run/.containerenv") or os.path.exists("/.dockerenv")


def host_command(*cmd):
    """argv to run a command on the SteamOS host from the dev container (as in
    display-settings: distrobox-host-exec needs the user's real session bus). It runs from the
    home folder: host-spawn starts the command in the caller's folder, and a container-only
    one such as /run/host/tmp doesn't exist on the host (every command then exits 127)."""
    exe = shutil.which("distrobox-host-exec")
    if not in_container() or not exe:
        return list(cmd)
    return ["env", "-C", os.path.expanduser("~"), "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/%d/bus" % os.getuid(),
            exe] + list(cmd)


def host_path(path):
    """A host file, from the container (/run/host) or the host itself."""
    for p in ("/run/host" + path, path):
        if os.path.exists(p):
            return p
    return None


def write_json(path, data):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(data, f, indent=1)
        f.write("\n")
    os.replace(tmp, path)


def clean_text(s):
    """Text for a panel command: one line; "|" is the panel's line break, so callers use it."""
    return " ".join(str(s).replace("\r", " ").replace("\n", " ").split())


# ------------------------------------------------------------------------------------------
# The camera ring (camd/fhring.h; tools/ring.py's layout, struct only)

RING_HDR = struct.Struct("<8sIIIIQqQ16x")              # 64 bytes
RING_CAM = struct.Struct("<32s32siIIIIIQQQQQIf24x")      # 160 bytes
RING_SLOT = struct.Struct("<QQQQQIf16x")                 # 64 bytes, the image follows
FH_CAM_DARK, FH_CAM_COLOR = 1, 2


class Ring:
    def __init__(self, path):
        fd = os.open(path, os.O_RDONLY)
        try:
            self.map = mmap.mmap(fd, 0, mmap.MAP_SHARED, mmap.PROT_READ)
        finally:
            os.close(fd)
        magic, version, _, ncams, _, _, self.writer_pid, _ = RING_HDR.unpack_from(self.map, 0)
        if magic != b"FHRING01" or version != 1:
            self.map.close()
            raise ValueError("%s isn't a camera ring" % path)
        self.cams = []
        for i in range(min(ncams, 8)):
            f = RING_CAM.unpack_from(self.map, RING_HDR.size + i * RING_CAM.size)
            self.cams.append({
                "index": i, "sensor": f[0].split(b"\0", 1)[0].decode(errors="replace"),
                "name": f[1].split(b"\0", 1)[0].decode(errors="replace"), "node": f[2],
                "width": f[4], "height": f[5], "stride": f[6], "nslots": f[7],
                "slot_offset": f[8], "slot_bytes": f[9], "flags": f[13]})

    def close(self):
        self.map.close()

    def alive(self, max_age=1.0):
        hb = struct.unpack_from("<Q", self.map, 40)[0]
        return hb != 0 and (mono_ns() - hb) / 1e9 < max_age

    def _cam_field(self, cam, fmt, off):
        return struct.unpack_from(fmt, self.map, RING_HDR.size + cam["index"] * RING_CAM.size + off)[0]

    def latest(self, cam):
        return self._cam_field(cam, "<Q", 104)

    def dark_mean(self, cam):
        return self._cam_field(cam, "<f", 132)

    def mono(self):
        """The mono tracking cameras (no dark twins, no colour cameras, also by name for
        ft-ringplay's rings, which carry no flags)."""
        return [c for c in self.cams if not c["flags"] & (FH_CAM_DARK | FH_CAM_COLOR)
                and not c["name"].endswith("_dk") and not c["name"].startswith("color")]

    def frame_mean(self, cam):
        """The newest frame's mean luma: ft-camd's from the slot header, else (ft-ringplay
        leaves it 0) measured on a sparse grid. None if there's no complete frame."""
        n = self.latest(cam)
        if not n:
            return None
        off = cam["slot_offset"] + (n % cam["nslots"]) * cam["slot_bytes"]
        seq, _, _, _, _, _, mean = RING_SLOT.unpack_from(self.map, off)
        if seq != 2 * n + 2:
            return None
        if mean <= 0:
            img, total, count = off + RING_SLOT.size, 0, 0
            for y in range(0, cam["height"], 16):
                row = self.map[img + y * cam["stride"]: img + y * cam["stride"] + cam["width"]: 16]
                total += sum(row)
                count += len(row)
            mean = total / count if count else 0.0
        if struct.unpack_from("<Q", self.map, off)[0] != seq:
            return None
        return float(mean)


def ring_lighting(ring_path=None, samples=5, interval=0.2):
    """Each mono camera's brightness now: {"<cam>": {"mean": float, "dark_mean": float}}, the
    newest frames' mean luma and ft-camd's near-black frame mean (the room's IR light), averaged
    over about a second. None if no camera ring is running."""
    path = ring_path or os.path.join(run_dir(), "cam-ring")
    try:
        ring = Ring(path)
    except (OSError, ValueError):
        return None
    try:
        if not ring.alive():
            return None
        sums = {}
        for k in range(samples):
            for cam in ring.mono():
                m = ring.frame_mean(cam)
                s = sums.setdefault(cam["name"], [0.0, 0, 0.0, 0])
                if m is not None:
                    s[0] += m
                    s[1] += 1
                s[2] += ring.dark_mean(cam)
                s[3] += 1
            if k + 1 < samples:
                time.sleep(interval)
        out = {name: {"mean": round(s[0] / s[1], 2) if s[1] else 0.0,
                      "dark_mean": round(s[2] / s[3], 2) if s[3] else 0.0} for name, s in sums.items()}
        return out or None
    finally:
        ring.close()


def _light_levels(ring):
    vals = [v for v in (ring or {}).values() if isinstance(v, dict)]
    if not vals:
        return None
    return (sum(float(v.get("mean", 0)) for v in vals) / len(vals),
            sum(float(v.get("dark_mean", 0)) for v in vals) / len(vals))


def _close(a, b, share=0.15, floor=1.0):
    """Within 15% of the larger; the floor keeps near-black levels (a dim room's dark mean is a
    few levels) from looking different over sensor noise."""
    return abs(a - b) <= max(share * max(abs(a), abs(b)), floor)


def similar_lighting(base_dir, lighting):
    """An earlier session whose cameras saw about the same light (the mean of every mono
    camera's mean and dark_mean, each within 15%): (session_id, chosen), the latest such, or
    None. lighting is ring_lighting()'s dict, or session.json's {"chosen", "ring"}."""
    if not lighting:
        return None
    ring = lighting.get("ring") if isinstance(lighting.get("ring"), dict) else lighting
    now = _light_levels(ring)
    if not now:
        return None
    for path in sorted(glob.glob(os.path.join(base_dir, "sessions", "*", "session.json")), reverse=True):
        try:
            with open(path) as f:
                light = json.load(f).get("lighting") or {}
        except (OSError, ValueError):
            continue
        then = _light_levels(light.get("ring"))
        if then and _close(now[0], then[0]) and _close(now[1], then[1]):
            return os.path.basename(os.path.dirname(path)), light.get("chosen", "")
    return None


DAYLIGHT_IR = 6.0   # ambient IR (the mono cameras' mean dark_mean) from which it's daylight; see classify_lighting


def ambient_ir(ring):
    """The mono cameras' mean dark_mean (the room's infrared light, as ft-hands logs it), or
    None if none has one. ring is ring_lighting()'s dict."""
    vals = [float(v.get("dark_mean", 0)) for v in (ring or {}).values() if isinstance(v, dict)]
    vals = [v for v in vals if v > 0]
    return round(sum(vals) / len(vals), 2) if vals else None


def classify_lighting(ring):
    """"daylight" or "indoor" from the room's infrared light, "" if it can't tell. Sunlight
    carries a lot of infrared; lamps and LEDs hardly any, so a dim room and a bright one read
    about the same (2026-10: 1.8 by one lamp, 2.2 in a lamp-lit room) and aren't told apart.
    Nor does a sunlit room reliably: the first daylight round (dataset PR #5, big sunlit windows)
    read 2.38, since the windows are a small part of each picture and the mean barely moves. So
    "indoor" means "no strong daylight on the cameras", and the window asks people to pick
    daylight themselves. Review corrects a missed one from the pictures (sunlit windows, the time
    of day): hands standing out little from the room goes with daylight but also with pale rooms
    at night, so it isn't a test either."""
    ir = ambient_ir(ring)
    if ir is None:
        return ""
    return "daylight" if ir >= DAYLIGHT_IR else "indoor"


def lighting_record(choice, ring):
    """session.json's "lighting": chosen is the person's pick, or with "auto" (or none) what
    the cameras measured; measured and ambient_ir are always the cameras' reading."""
    measured = classify_lighting(ring)
    picked = choice if choice in ("dim", "room", "daylight") else ""
    return {"chosen": picked or measured, "source": "picked" if picked else "measured",
            "measured": measured, "ambient_ir": ambient_ir(ring), "ring": ring or {}}


def unit_active(unit):
    return subprocess.run(host_command("systemctl", "--user", "-q", "is-active", unit),
                          capture_output=True, timeout=30).returncode == 0


def start_unit(unit, what, argv, log=lambda line: None):
    """Start argv as a transient user unit that stops with SteamVR. True if it started it,
    False if it was running already; raises RuntimeError if it couldn't."""
    if unit_active(unit):
        log("%s is running already" % unit)
        return False
    cmd = host_command("systemd-run", "--user", "--quiet", "--collect", "--unit=" + unit,
                       "--description=Frametop hand recorder: " + what,
                       "-p", "PartOf=steamvr.service", "-p", "After=steamvr.service",
                       "-p", "Restart=on-failure", "-p", "RestartSec=3", "-p", "TimeoutStopSec=5", *argv)
    for attempt in range(3):   # distrobox-host-exec has failed once, silently, and worked again
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
        if r.returncode == 0 or unit_active(unit):
            log("started %s" % unit)
            return True
        log("starting %s failed (exit %d): %s" % (unit, r.returncode, (r.stderr or r.stdout).strip()))
        time.sleep(0.5)
    raise RuntimeError("couldn't start %s (exit %d): %s" % (unit, r.returncode, (r.stderr or r.stdout).strip()))


def stop_unit(unit):
    subprocess.run(host_command("systemctl", "--user", "stop", unit), capture_output=True, timeout=30)


def ring_alive(path=None):
    try:
        r = Ring(path or os.path.join(run_dir(), "cam-ring"))
    except (OSError, ValueError):
        return False
    try:
        return r.alive()
    finally:
        r.close()


def start_camd(path=None, log=lambda line: None, stop=lambda: False, timeout=15.0):
    """Make sure ft-camd fills the camera ring: start it (CAMD_UNIT) if nothing does. True if
    it started it, False if a ring was live already; raises RuntimeError if it can't."""
    path = path or os.path.join(run_dir(), "cam-ring")
    if ring_alive(path):
        return False
    if not os.access(FT_CAMD, os.X_OK):
        raise RuntimeError("ft-camd isn't built: hands/build.sh")
    caps = subprocess.run(["getcap", FT_CAMD], capture_output=True, text=True) if shutil.which("getcap") else None
    if caps is not None and "cap_sys_ptrace" not in caps.stdout:
        raise RuntimeError("ft-camd needs its capabilities: hands/run.sh caps (asks for sudo)")
    started = start_unit(CAMD_UNIT, "the camera broker", [FT_CAMD, "--status", "60"], log)
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if ring_alive(path):
            return started
        if stop():
            return started
        time.sleep(0.1)
    raise RuntimeError("ft-camd didn't start (is SteamVR running?): journalctl --user -u " + CAMD_UNIT)


# ------------------------------------------------------------------------------------------
# The factory calibration, without what identifies the unit

ID_TOKENS = {"serial", "sn", "uuid", "guid", "mac", "id", "ids", "identifier"}


def _key_identifies(key):
    k = str(key)
    if re.search(r"serial|uuid", k, re.I):
        return True
    tokens = re.findall(r"[A-Z]+(?![a-z])|[A-Z]?[a-z]+|\d+", k)
    return any(t.lower() in ID_TOKENS for t in tokens)


def _value_identifies(value):
    """A string holding something like a serial number: a run of 8 or more letters and digits
    with at least 4 digits and a letter (sensor names like og01a1b are shorter)."""
    if not isinstance(value, str):
        return False
    for run in re.findall(r"[A-Za-z0-9]+", value):
        if len(run) >= 8 and sum(c.isdigit() for c in run) >= 4 and any(c.isalpha() for c in run):
            return True
    return False


def strip_calibration(xrservice_json_dict):
    """/persist/xrservice.json without what identifies the unit: keys naming a serial, sn,
    uuid, mac or id, and string values that look like serial numbers. Returns (cleaned dict,
    removed key paths such as "cameras[0].serial")."""
    removed = []

    def walk(o, path):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                p = "%s.%s" % (path, k) if path else str(k)
                if _key_identifies(k) or _value_identifies(v):
                    removed.append(p)
                    continue
                out[k] = walk(v, p)
            return out
        if isinstance(o, list):
            out = []
            for i, v in enumerate(o):
                p = "%s[%d]" % (path, i)
                if _value_identifies(v):
                    removed.append(p)
                    out.append(None)   # keep the other items' positions
                    continue
                out.append(walk(v, p))
            return out
        return o

    return walk(xrservice_json_dict, ""), removed


# ------------------------------------------------------------------------------------------
# The live hands file (include/fh_hands.h)

HANDS_HDR = struct.Struct("<8sIIQQQII16x")    # 64 bytes
HAND = struct.Struct("<IIff63fI")             # 272 bytes
FH_HAND_RIGHT = 1
PALM = (0, 5, 9, 13, 17)                      # wrist and knuckles
INDEX_TIP = 8


class HandsFile:
    """Reads ft-hands' hands file under its sequence lock. read() gives None while no tracker
    publishes (no file, or nothing for FRESH_S), else {"left": hand|None, "right": hand|None}
    with hand = {"palm": [x,y,z], "palm_m": float, "tip": [x,y,z]} in the head frame."""

    def __init__(self, path):
        self.path = path

    def read(self):
        try:
            fd = os.open(self.path, os.O_RDONLY)
        except OSError:
            return None
        try:
            size = HANDS_HDR.size + 2 * HAND.size
            for _ in range(4):
                data = os.pread(fd, size, 0)
                if len(data) < size:
                    return None
                magic, version, _, seq, _, publish_ns, nhands, _ = HANDS_HDR.unpack_from(data, 0)
                if magic != b"FHHANDS1" or version != 1:
                    return None
                if seq % 2 or os.pread(fd, 8, 16) != data[16:24]:
                    continue
                if (mono_ns() - publish_ns) / 1e9 > FRESH_S:
                    return None
                out = {"left": None, "right": None}
                for k in range(min(nhands, 2)):
                    f = HAND.unpack_from(data, HANDS_HDR.size + k * HAND.size)
                    side = "right" if f[1] & FH_HAND_RIGHT else "left"
                    if out[side]:
                        continue
                    pts = [f[4 + 3 * i: 7 + 3 * i] for i in range(21)]
                    palm = [sum(pts[i][j] for i in PALM) / len(PALM) for j in range(3)]
                    out[side] = {"palm": palm, "palm_m": math.sqrt(sum(v * v for v in palm)),
                                 "tip": list(pts[INDEX_TIP])}
                return out
            return None
        finally:
            os.close(fd)


# ------------------------------------------------------------------------------------------
# The panel (ft-handpanel, @ft_handpanel)

class Panel:
    """Commands to ft-handpanel over its datagram socket. Commands that need an answer wait
    for it; the rest don't, and their replies are read (and errors logged) later. With
    dry=True nothing is sent: commands are printed and replies made up."""

    def __init__(self, name=PANEL_SOCKET, dry=False, log=None, out=None):
        self.name, self.dry, self.log, self.out = name, dry, log or (lambda s: None), out or print
        self.sock = None
        self.sent = {}
        self.pending = 0   # commands sent and not answered yet
        if not dry:
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
            self.sock.bind("\0ft_handrec.%d.%d" % (os.getpid(), id(self) & 0xffff))
            self.sock.setblocking(False)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def _drain(self):
        while self.sock:
            try:
                r = self.sock.recv(4096).decode(errors="replace")
            except (BlockingIOError, OSError):
                return
            self.pending = max(0, self.pending - 1)
            if r.startswith("error"):
                self.log("panel: %s" % r)

    def cmd(self, text, reply=False, timeout=0.5):
        """Send a command; with reply=True, its answer ("ok ..." or "error ..."), or None.
        The panel answers every command in order, so the answer is the one after those of
        the commands still unanswered."""
        if self.dry:
            self.out("panel: %s" % text)
            if not reply:
                return None
            if text.startswith("devices"):
                return "ok hmd 200 left 200 right 200"
            return "ok shown" if text == "ping" else "ok"
        if not self.sock:
            return None
        self._drain()
        try:
            self.sock.sendto(text.encode(), "\0" + self.name)
        except OSError:
            return None
        self.pending += 1
        if not reply:
            return None
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            try:
                self.sock.settimeout(max(0.01, end - time.monotonic()))
                r = self.sock.recv(4096).decode(errors="replace")
            except OSError:
                break
            finally:
                self.sock.setblocking(False)
            self.pending = max(0, self.pending - 1)
            if self.pending == 0:
                return r
            if r.startswith("error"):
                self.log("panel: %s" % r)
        self.pending = 0   # a lost answer: start counting afresh
        return None

    def set(self, key, text):
        """Send a command only if it changes what that part of the panel shows."""
        if self.sent.get(key) == text:
            return
        self.sent[key] = text
        self.cmd(text)


# ------------------------------------------------------------------------------------------
# Processes

def find_processes(name):
    """[(pid, argv)] of running processes called name (the container shares the host's PIDs)."""
    out = []
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % pid) as f:
                if f.read().strip() != name:
                    continue
            with open("/proc/%s/cmdline" % pid, "rb") as f:
                argv = [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
        except OSError:
            continue
        out.append((int(pid), argv))
    return out


def tracker_running():
    """A tracking ft-hands (not a --record-only recorder)."""
    return any("--record-only" not in argv for _, argv in find_processes("ft-hands"))


class Recorder:
    """One part of a take's recording: ft-hands --record-only as a child process. Part 1
    writes TAKE/sets.bin; part N (after a pause) records into TAKE/.part-N and is moved to
    TAKE/sets-N.bin when it ends (ft-hands never overwrites, and always writes DIR/sets.bin)."""

    def __init__(self, take_dir, part, seconds, ring, log_file, swap=None):
        """swap: the side cameras' decision (sides.py), passed on as --sides 1 or 0; None (not
        known yet) records them as ft-camd names them (--sides auto, whatever the config says)."""
        self.take_dir, self.part = take_dir, part
        self.dir = take_dir if part == 1 else os.path.join(take_dir, ".part-%d" % part)
        self.file = "sets.bin" if part == 1 else "sets-%d.bin" % part
        self.names_swapped = bool(swap)
        argv = [FT_HANDS, "--record-only", "--record", self.dir, "--record-for", "%.0f" % max(seconds, 5),
                "--record-hz", str(RECORD_HZ), "--status", "0",
                "--sides", "auto" if swap is None else "1" if swap else "0"]
        if ring:
            argv += ["--ring", ring]
        if not in_container():
            argv = [os.path.expanduser("~/.local/bin/distrobox"), "enter", "dev", "--"] + argv
        self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=log_file, stderr=log_file)
        self.started_ns = mono_ns()

    def exited(self):
        return self.proc.poll() is not None

    def has_data(self):
        """It has written a set (ft-hands starts recording within a few tens of milliseconds)."""
        try:
            return os.path.getsize(os.path.join(self.dir, "sets.bin")) > 0
        except OSError:
            return False

    def stop(self):
        """End it (SIGTERM: ft-hands writes out its queue) and put the part in place. Afterwards
        names_swapped is what ft-hands says it applied (its DIR/sides.json, which goes into
        take.json's "parts" and is removed here)."""
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(15)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        side_file = os.path.join(self.dir, "sides.json")
        try:
            with open(side_file) as f:
                runs = json.load(f).get("names_swapped") or []
            if runs:
                self.names_swapped = bool(runs[0][1])   # --record-only never changes it
            os.remove(side_file)
        except (OSError, ValueError, TypeError, IndexError):
            pass
        if self.part > 1:
            src = os.path.join(self.dir, "sets.bin")
            if os.path.exists(src):
                os.replace(src, os.path.join(self.take_dir, "sets-%d.bin" % self.part))
            try:
                os.rmdir(self.dir)
            except OSError:
                pass
        return self.proc.returncode


# ------------------------------------------------------------------------------------------
# The headset's button, and mice (/proc/bus/input/devices, linux/input.h)
#
# The Frame's click button on its right side is KEY_SELECT on the "gpio-keys" device. It's read
# as any other reader does, never grabbed (EVIOCGRAB): Frametop's input relay, ft-powerd, SteamVR
# and gamescope read the same device, and the relay remaps its volume keys.

INPUT_DEVICES = "/proc/bus/input/devices"
INPUT_EVENT = struct.Struct("@llHHi")   # struct input_event: a timeval, type, code, value (24 bytes on 64-bit)
EV_KEY, EV_REL = 0x01, 0x02
REL_X, REL_Y = 0x00, 0x01
KEY_SELECT = 353
BUTTON_DEVICE = "gpio-keys"
BUTTON_DEBOUNCE_S = 0.3   # presses closer than this count once


def parse_input_devices(text):
    """/proc/bus/input/devices as [{"name", "phys", "sysfs", "bus", "handlers": [...], "bits": {"EV": int, ...}}]."""
    out, dev = [], None
    for line in text.splitlines() + [""]:
        line = line.strip()
        if not line:
            if dev:
                out.append(dev)
            dev = None
            continue
        if dev is None:
            dev = {"name": "", "phys": "", "sysfs": "", "bus": 0, "handlers": [], "bits": {}}
        tag, _, rest = line.partition(": ")
        if tag == "I":
            m = re.search(r"Bus=([0-9a-fA-F]+)", rest)
            dev["bus"] = int(m.group(1), 16) if m else 0
        elif tag == "N":
            dev["name"] = rest.partition("=")[2].strip('"')
        elif tag == "P":
            dev["phys"] = rest.partition("=")[2]
        elif tag == "S":
            dev["sysfs"] = rest.partition("=")[2]
        elif tag == "H":
            dev["handlers"] = rest.partition("=")[2].split()
        elif tag == "B":
            key, _, words = rest.partition("=")
            value = 0
            for w in words.split():   # the highest long first
                try:
                    value = (value << 64) | int(w, 16)
                except ValueError:
                    value = 0
                    break
            dev["bits"][key] = value
    return out


def _event_node(dev):
    for h in dev["handlers"]:
        if re.fullmatch(r"event\d+", h):
            return "/dev/input/" + h
    return None


def find_button(devices):
    """The headset button's event device: "gpio-keys" with KEY_SELECT, or None."""
    for dev in devices:
        if dev["name"] == BUTTON_DEVICE and dev["bits"].get("KEY", 0) >> KEY_SELECT & 1:
            return _event_node(dev)
    return None


def real_mouse(dev):
    """A pointing device a person holds: relative X and Y, not made in software. uinput devices
    (Frametop's virtual mouse, frame-voice's keyboard) sit under /devices/virtual/input or on the
    virtual bus (6); Bluetooth mice come through uhid, also under /devices/virtual, so those count."""
    bits = dev["bits"]
    if not (bits.get("EV", 0) >> EV_REL & 1) or (bits.get("REL", 0) & 3) != 3:   # REL_X and REL_Y
        return False
    sysfs = dev["sysfs"]
    if dev["bus"] == 0x06 or "virtual" in dev["name"].lower() or "uinput" in dev["phys"]:
        return False
    return not sysfs.startswith("/devices/virtual/") or sysfs.startswith("/devices/virtual/misc/uhid/")


def read_input_devices(path=INPUT_DEVICES):
    try:
        with open(path) as f:
            return parse_input_devices(f.read())
    except OSError:
        return []


def mouse_connected(path=INPUT_DEVICES):
    return any(real_mouse(d) for d in read_input_devices(path))


def button_presses(data):
    """KEY_SELECT key-downs in a run of input_event structs (value 1; releases and autorepeat
    are left out). Returns (how many, the bytes left over after the last whole event)."""
    n, usable = 0, len(data) - len(data) % INPUT_EVENT.size
    for off in range(0, usable, INPUT_EVENT.size):
        _, _, etype, code, value = INPUT_EVENT.unpack_from(data, off)
        if etype == EV_KEY and code == KEY_SELECT and value == 1:
            n += 1
    return n, data[usable:]


class ButtonReader:
    """Reads the headset button on a thread and calls on_press() per press, debounced. path:
    an event device or, for testing, a FIFO carrying input_event structs. It's opened read-only
    and shared; if it can't be opened (no device, no permission, /dev/input not reachable in a
    container) it says so in the log and tries again now and then."""

    def __init__(self, path, on_press, log=None, debounce_s=BUTTON_DEBOUNCE_S):
        self.path, self.on_press, self.log = path, on_press, log or (lambda s: None)
        self.debounce_s = debounce_s
        self.ok = False      # opened at least once
        self._stop = threading.Event()
        self._last = -1e9
        self._thread = threading.Thread(target=self._run, name="handrec-button", daemon=True)

    def start(self):
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        self._thread.join(2)

    def _press(self, n):
        now = time.monotonic()
        if n and now - self._last >= self.debounce_s:
            self._last = now
            self.on_press()

    def _run(self):
        failed = False
        while not self._stop.is_set():
            try:
                fd = os.open(self.path, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
            except OSError as e:
                if not failed:
                    self.log("headset button: can't open %s (%s): the window's keys still work" % (self.path, e.strerror))
                failed = True
                self._stop.wait(5)
                continue
            if failed or not self.ok:
                self.log("headset button: reading %s" % self.path)
            self.ok, failed = True, False
            rest = b""
            try:
                while not self._stop.is_set():
                    r, _, _ = select.select([fd], [], [], 0.2)
                    if not r:
                        continue
                    try:
                        data = os.read(fd, INPUT_EVENT.size * 64)
                    except BlockingIOError:
                        continue
                    if not data:   # a FIFO's writer left: open it again
                        self._stop.wait(0.2)
                        break
                    n, rest = button_presses(rest + data)
                    self._press(n)
            except OSError as e:   # the device went away
                self.log("headset button: %s: %s" % (self.path, e.strerror))
                self._stop.wait(2)
            finally:
                os.close(fd)


# ------------------------------------------------------------------------------------------
# The pose pictures (poses/poses.json: {"<pose>": {"file": "<name>.png", "two_hands", "caption"}})

def load_poses(poses_dir):
    """{pose id: {"path", "two_hands", "caption"}} from poses_dir/poses.json, or {} without one."""
    try:
        with open(os.path.join(poses_dir, "poses.json")) as f:
            raw = json.load(f)
    except (OSError, ValueError):
        return {}
    out = {}
    for pose, e in (raw.items() if isinstance(raw, dict) else ()):
        if isinstance(e, dict) and isinstance(e.get("file"), str) and e["file"]:
            out[pose] = {"path": os.path.join(poses_dir, os.path.basename(e["file"])),
                         "two_hands": bool(e.get("two_hands")), "caption": clean_text(e.get("caption", ""))}
    return out


def pose_view(poses, p):
    """A prompt's picture: (path, mode, caption), mode "" (as drawn: a right hand, or both hands),
    "mirror" (a left hand) or "both" (a mirrored copy on the left, the picture on the right).
    The prompt's "picture" (a pose id) picks a picture other than its pose's, and
    "picture_mirror" mirrors a two-hand picture. ("", "", "") when there's none."""
    e = poses.get(p.get("picture") or p.get("pose") or "")
    if not e or not os.path.isfile(e["path"]):
        return "", "", ""
    hands = p.get("hands")
    if e["two_hands"]:
        mode = "mirror" if p.get("picture_mirror") else ""
    else:
        mode = "mirror" if hands == "left" else "both" if hands == "both" else ""
    return e["path"], mode, e["caption"]


# ------------------------------------------------------------------------------------------
# The script

HAND_VALUES = ("left", "right", "both", "none", "any", "")
PROMPT_KEYS = ("text", "seconds", "hands", "pose", "distance", "position", "object", "controller")


def _when_ok(cond, ctx):
    """A prompt's or block's "when": "controllers", "objects:keyboard", either negated with "!",
    or a list of them, all to hold."""
    if not cond:
        return True
    if isinstance(cond, list):   # all of them
        return all(_when_ok(c, ctx) for c in cond)
    neg = cond.startswith("!")
    c = cond.lstrip("!")
    value = c[len("objects:"):] in ctx["objects"] if c.startswith("objects:") else bool(ctx.get(c))
    return value != neg


def _prompt(section, raw, ctx, subst=None):
    p = dict(section.get("defaults") or {})
    p.update(raw)
    p.pop("when", None)
    for k, v in (subst or {}).items():
        for key in ("text", "object"):
            if isinstance(p.get(key), str):
                p[key] = p[key].replace("{%s}" % k, v)
    if subst and "object_key" in subst:
        p["object"] = subst["object_key"]
    for key in PROMPT_KEYS:
        p.setdefault(key, False if key == "controller" else 0 if key == "seconds" else "")
    p["controllers"] = list(p.get("controllers") or [])
    if p["hands"] not in HAND_VALUES:
        raise ValueError("%s: hands %r" % (section["id"], p["hands"]))
    return p


def _set_ids(section, prompts):
    seen = {}
    for p in prompts:
        if not p.get("id"):
            parts = [section["id"]] + [re.sub(r"[^a-z0-9]+", "-", str(p[k]).lower()).strip("-")
                                       for k in ("pose", "hands", "distance", "position", "object") if p[k]]
            p["id"] = "/".join(parts) if len(parts) > 1 else "%s/%d" % (section["id"], len(seen) + 1)
        n = seen.get(p["id"], 0) + 1
        seen[p["id"]] = n
        if n > 1:
            p["id"] = "%s#%d" % (p["id"], n)


def load_script(path):
    with open(path) as f:
        script = json.load(f)
    if script.get("version") != 1 or not isinstance(script.get("sections"), list):
        raise ValueError("%s: not a version 1 script" % path)
    return script


def session_seed(session_id):
    """The shuffle's seed for a session: from its id, so the same id gives the same plan."""
    return int(hashlib.sha256(session_id.encode()).hexdigest()[:8], 16)


def _sweep_prompts(s, raw, ctx, rng):
    """A sweep section's steps (DESIGN.md, "Sweeps"): each a prompt with "cues", the pose of each
    cue_s slot in order. A step names its cues, or takes a group: "next" (the groups in turn, in
    the shuffled order) or "any" (a group drawn at random, different for each "any" while there
    are groups left). With rng (the session's shuffle) the groups' order, the "any" draws and the
    cues' order within a step are shuffled, unless the section says "shuffle": false or the
    step "fixed": true; without it, everything goes in the script's order."""
    groups = [list(g) for g in raw.get("groups") or []]
    shuffle = rng is not None and raw.get("shuffle", True)
    order = list(range(len(groups)))
    pool = list(range(len(groups)))
    if shuffle:
        rng.shuffle(order)
        rng.shuffle(pool)
    taken, drawn, counts = 0, 0, {}
    out = []
    for step in raw.get("sweeps") or []:
        if not _when_ok(step.get("when"), ctx):
            continue
        g = step.get("group")
        if step.get("cues"):
            cues = list(step["cues"])
        elif g == "next" and groups:
            cues, taken = list(groups[order[taken % len(order)]]), taken + 1
        elif g == "any" and groups:
            cues, drawn = list(groups[pool[drawn % len(pool)]]), drawn + 1
        elif isinstance(g, int) and 0 <= g < len(groups):
            cues = list(groups[g])
        else:
            raise ValueError("%s: a sweep needs cues or a group" % s["id"])
        if shuffle and not step.get("fixed"):
            rng.shuffle(cues)
        cue_s = float(step.get("cue_s", raw.get("cue_s", 4)))
        slots = max(len(cues), int(math.ceil(float(step.get("seconds", raw.get("step_s", cue_s * len(cues)))) / cue_s
                                             - 1e-9))) if step.get("cycle", raw.get("cycle", True)) else len(cues)
        p = _prompt(s, {k: v for k, v in step.items() if k not in ("group", "cues", "fixed", "cycle", "quick")}, ctx)
        p["cues"] = [cues[k % len(cues)] for k in range(slots)]
        p["cue_s"] = cue_s
        p["seconds"] = cue_s * slots
        p["pose"] = p["cues"][0]
        if not step.get("id"):
            counts[p["hands"]] = counts.get(p["hands"], 0) + 1
            p["id"] = "%s/%s-%d" % (s["id"], p["hands"] or "any", counts[p["hands"]])
        else:
            p["id"] = "%s/%s" % (s["id"], step["id"])
        if step.get("quick") is False:
            p["quick"] = False
        out.append(p)
    return out


def build_plan(script, checklist, seed=None, quick=False):
    """The sections this session runs, prompts expanded for the checklist, and the ones
    skipped: (plan, skipped) with skipped = [{"section", "reason"}]. seed: the shuffle of the
    sweeps (session_seed; None: the script's order). quick: the quick round, only sections
    marked "quick" and, in those, no step marked "quick": false."""
    objects = [o for o in (checklist.get("objects") or []) if o]
    own = [o for o in (checklist.get("own_objects") or []) if str(o).strip()]
    ctx = {"objects": set(objects) | set(own), "controllers": checklist.get("controllers") == "straps"}
    names = script.get("objects") or {}
    plan, skipped = [], []
    for raw in script["sections"]:
        sid = raw["id"]
        if quick and not raw.get("quick"):
            skipped.append({"section": sid, "reason": "not in a quick round"})
            continue
        missing = [r for r in raw.get("requires") or [] if not ctx.get(r)]
        if missing:
            skipped.append({"section": sid, "reason": "needs " + ", ".join(missing)})
            continue
        s = dict(raw)
        s["kind"] = raw.get("kind", "prompts")
        s["intro_s"] = float(raw.get("intro_s", script.get("intro_s", 4)))
        prompts = []
        if s["kind"] == "sweep":
            rng = random.Random("%d/%s" % (seed, sid)) if seed is not None else None
            prompts = _sweep_prompts(s, raw, ctx, rng)
            if quick:
                prompts = [p for p in prompts if p.get("quick") is not False]
        elif raw.get("for_each") == "object":
            skip = set(raw.get("skip_objects") or [])
            for o in [o for o in objects if o not in skip] + own:
                label = clean_text(names.get(o, o)).replace("|", "/")
                for p in raw.get("prompts") or []:
                    if _when_ok(p.get("when"), ctx):
                        prompts.append(_prompt(s, p, ctx, {"object": label, "object_key": clean_text(o)}))
        else:
            prompts = [_prompt(s, p, ctx) for p in raw.get("prompts") or [] if _when_ok(p.get("when"), ctx)
                       and not (quick and p.get("quick") is False)]
            for p in prompts:   # a prompt may have cues too (hand size): its own, in its order
                if p.get("cues"):
                    cue_s = float(p.get("cue_s", raw.get("cue_s", 4)))
                    p["cues"], p["cue_s"] = list(p["cues"]), cue_s
                    p["seconds"] = cue_s * len(p["cues"])
                    p["pose"] = p["cues"][0]
        _set_ids(s, prompts)
        s["prompts"] = prompts
        if s["kind"] == "bar":
            heights = []
            for h in raw.get("heights") or []:
                h = {"id": h} if isinstance(h, str) else dict(h)
                h.setdefault("text", (raw.get("height_text") or {}).get(h["id"], raw.get("text", "")))
                h.setdefault("reps", raw.get("reps", 5))
                heights.append(h)
            s["heights"] = heights
        before = raw.get("before")
        s["before"] = before if before and _when_ok(before.get("when"), ctx) else None
        if s["kind"] in ("prompts", "sweep") and not prompts:
            skipped.append({"section": sid, "reason": "nothing to do"})
            continue
        if s["kind"] == "targets" and not raw.get("targets"):
            skipped.append({"section": sid, "reason": "no targets"})
            continue
        plan.append(s)
    return plan, skipped


def plan_record(plan):
    """What the shuffle chose, for session.json: each section's sweep steps and their cues."""
    out = {}
    for s in plan:
        steps = [{"id": p["id"], "hands": p["hands"], "cues": p["cues"]} for p in s["prompts"] if p.get("cues")]
        if steps:
            out[s["id"]] = steps
    return out


def section_steps(s):
    """The steps that wait for Next in step mode: each prompt and bar height, and the first target."""
    return (len(s["prompts"]) + (len(s.get("heights") or []) if s["kind"] == "bar" else 0)
            + (1 if s["kind"] == "targets" and s.get("targets") else 0))


def section_seconds(s, worst=False, auto=True):
    """A section's length in script seconds (targets: about 4 s each, or the timeout if worst).
    In step mode, what's recorded: a countdown before each step and the holds; the intro and
    the waits for Next aren't."""
    t = (s["intro_s"] if auto else COUNTDOWN_S * section_steps(s)) + sum(p["seconds"] for p in s["prompts"])
    if s["kind"] == "targets":
        t += len(s["targets"]) * (s.get("timeout_s", 8) if worst else min(4.0, s.get("timeout_s", 8)))
    if s["kind"] == "bar":
        lead = s.get("lead_s", 3) if auto else 0
        t += sum(lead + h["reps"] * s.get("period_s", 6) for h in s["heights"])
    return t


def plan_seconds(script, plan, worst=False, auto=True):
    """The session's length: in auto mode all of it; in step mode only what's recorded, as the
    time spent reading each step before pressing Next is up to the person (plan_steps)."""
    if not auto:
        return sum(section_seconds(s, worst, auto=False) for s in plan)
    t = (script.get("welcome") or {}).get("seconds", 0) + (script.get("done") or {}).get("seconds", 0)
    for i, s in enumerate(plan):
        t += section_seconds(s, worst)
        t += s["before"]["seconds"] if s.get("before") else (script.get("between_s", 3) if i else 0)
    return t


def plan_steps(plan):
    return sum(section_steps(s) for s in plan)


def plan_summary(script, plan, auto=False):
    """The length in words, for the window and --plan (the plan of a quick round or a full one)."""
    minutes = max(1, round(plan_seconds(script, plan, auto=auto) / 60))
    if auto:
        return "about %d min, each step advancing by itself" % minutes
    n = plan_steps(plan)
    return ("about %d min of recording in %d steps, plus the time you take to read each step before "
            "pressing Next (at 5 s a step, about %d min more)" % (minutes, n, max(1, round(n * 5 / 60))))


# ------------------------------------------------------------------------------------------
# The session

class _Skip(Exception):
    pass


class _Stop(Exception):
    pass


class _Fail(Exception):
    pass


class _Redo(Exception):
    pass


def _git_describe():
    try:
        r = subprocess.run(["git", "-C", REPO, "describe", "--always", "--dirty", "--tags"],
                           capture_output=True, text=True, timeout=5)
        return r.stdout.strip() or "unknown"
    except (OSError, subprocess.TimeoutExpired):
        return "unknown"


def _os_version():
    path = "/run/host/etc/os-release" if in_container() else "/etc/os-release"
    try:
        with open(path) as f:
            for line in f:
                if line.startswith("VERSION_ID="):
                    return line.split("=", 1)[1].strip().strip('"')
    except OSError:
        pass
    return ""


def _steamvr_version():
    for db in ("/usr/lib/holo/pacmandb", "/var/lib/pacman"):   # SteamOS keeps it in the image
        found = sorted(glob.glob(("/run/host" if in_container() else "") + db + "/local/deckard-steamvr-rel-*"))
        if found:
            return os.path.basename(found[-1])[len("deckard-steamvr-rel-"):]
    p = host_path("/opt/steamvr/bin/version.txt")
    if p:
        try:
            with open(p) as f:
                return "build " + f.read().strip()
        except OSError:
            pass
    return ""


_camd_restarted = set()   # ft-camd pids camera_check(repair=True) has restarted: once each


def _unit_pid(unit):
    r = subprocess.run(host_command("systemctl", "--user", "show", "-p", "MainPID", "--value", unit),
                       capture_output=True, text=True, timeout=30)
    try:
        return int(r.stdout.strip() or 0)
    except ValueError:
        return 0


def camera_check(repair=False):
    """camcheck.check() (the XRService log, its open cameras where readable, ft-camd's ring);
    never raises: a failure is "unknown". repair (the window, never during a session): when
    XRService runs every tracking camera but the recorder's own ft-camd doesn't publish them all
    (it started while some were missing), restart it, once per ft-camd, and check again."""
    try:
        r = camcheck.check()
        pid = (r.get("ring") or {}).get("writer_pid")
        if repair and camcheck.is_ring_short(r) and pid not in _camd_restarted and _unit_pid(CAMD_UNIT) == pid:
            _camd_restarted.add(pid)
            stop_unit(CAMD_UNIT)
            start_camd()
            r = camcheck.check()
            r["evidence"].append("restarted ft-camd (pid %d) because it published only some of the cameras" % pid)
        return r
    except Exception as e:
        return {"status": "unknown", "summary": "unknown: the camera check failed (%s)" % e,
                "reason": str(e), "evidence": []}


def camera_text(result):
    """What to tell the person about a camera check that isn't ok ("" if it's ok or unknown)."""
    if not result or result.get("status") != "degraded":
        return ""
    if camcheck.is_vcint_failure(result):
        return camcheck.USER_TEXT
    if camcheck.is_ring_short(result):
        return ("The headset's tracking cameras are all running, but the recorder can't read some of them (%s). "
                "Close the Hand Recorder, run ~/frametop/hands/rec/install.sh again, and open it again. If that "
                "doesn't help, ask in the Frametop Discord." % result.get("reason", ""))
    return ("Not all of the headset's tracking cameras are running (%s). Restart SteamVR, or restart the "
            "headset if that doesn't fix it." % result.get("reason", ""))


class Session:
    """One recording session. start() runs it in its own thread; next_step(), pause(), resume(),
    redo(), skip() and stop() steer it from any thread. on_status(dict) is called from the
    session thread whenever something changes (the keys of _status).

    auto=False is step mode: each step waits for next_step(), then a recorded countdown, then
    the hold; nothing is recorded while it waits. auto=True: the timed flow, each prompt
    advancing by itself, one recording per take. next_after (a test hook): press Next by itself
    after that many seconds of waiting. poses_dir: the pose pictures (poses/). button: read the
    headset's button (not in a dry run unless button_device, a test hook, names the device or
    a FIFO of input_event structs). check_cameras: run the camera check first and don't start
    if it fails (not in a dry run, nor with ring). hands_reader (a test hook): an object whose
    read() stands in for the hands file, read in a dry run too. quick: the quick round (a few
    sections, about 3 min). seed: the sweeps' shuffle (default: from the session's id)."""

    def __init__(self, base_dir, profile, checklist, lighting_choice, script_path, *, ring=None,
                 start_processes=True, dry_run=False, speed=1.0, on_status=None, hands_dir=None, panel_bin=None,
                 auto=False, next_after=None, poses_dir=None, button=True, button_device=None,
                 check_cameras=True, hands_reader=None, quick=False, seed=None):
        self.base_dir = os.path.abspath(os.path.expanduser(base_dir or BASE_DIR))
        self.profile = dict(profile or {})
        self.checklist = dict(checklist or {})
        self.lighting_choice = lighting_choice or ""
        self.script_path = script_path or SCRIPT_PATH
        self.ring = ring
        self.start_processes = start_processes
        self.dry_run = dry_run
        self.speed = max(float(speed or 1.0), 0.01)
        self.on_status = on_status
        self.hands_dir = hands_dir or run_dir()
        self.panel_bin = panel_bin or PANEL_BIN
        self.auto = bool(auto)
        self.next_after = next_after
        self.button, self.button_device = bool(button), button_device
        self.check_cameras = bool(check_cameras)
        self.quick = bool(quick)
        self.seed = seed   # set from the session's id when it starts, unless given
        # the camera check (a test hook: tests swap it); a dry run looks at no real cameras
        self.camera_check_fn = (lambda: None) if dry_run else camera_check
        self.input_devices = INPUT_DEVICES   # where mice are looked for (a test hook)
        self._button = None                  # the ButtonReader
        self.print = print   # where dry-run panel commands go (the CLI's stdout)
        self.script = load_script(self.script_path)
        # the plan in the script's order; the session's own shuffle comes with its id (_make_dir)
        self.plan, self.skipped = build_plan(self.script, self.checklist, seed=seed, quick=self.quick)
        self._poses = load_poses(poses_dir or POSES_DIR)
        self.session_dir = ""
        self._thread = None
        self._lock = threading.Lock()
        self._want = {"pause": False, "skip": False, "stop": False, "next": False, "redo": False}
        self._wake = threading.Event()
        # state: starting, intro, ready (a step waits for Next), countdown, running (a hold),
        # between, paused, done, stopped, error. waiting: Next is wanted. big: the countdown's
        # number, then the hold's word ("Hold", "Go"), as the panel shows them. The picture and the
        # diagram: image (a path or ""), image_mode ("", "mirror", "both"), position, distance.
        self._status = {"state": "starting", "mode": "auto" if self.auto else "step", "section": "", "title": "",
                        "section_index": 0, "section_count": len(self.plan), "step_index": 0, "step_count": 0,
                        "prompt": "", "seconds_left": 0.0, "note": "", "hands": {"left": None, "right": None},
                        "take": None, "error": "", "waiting": False, "countdown": 0, "big": "", "can_redo": False,
                        "image": "", "image_mode": "", "caption": "", "position": "", "distance": "",
                        "ready_text": READY_TEXT, "button": False, "mouse": True,
                        "camera": None, "nohands": False, "strip": [], "cue": -1, "quick": self.quick}
        self._last_emit = 0.0
        self._log_file = None
        self._panel = None
        self._panel_proc = None
        self._units = []           # transient units this session started
        self._session_json = None
        self._take = None          # the take in progress: dict
        self._recorder = None
        self._prompt = None        # the prompt shown: dict (hands, controllers)
        self._live = None          # the hands file's last read
        self._hands_file = hands_reader or HandsFile(os.path.join(self.hands_dir, "hands"))
        self._read_hands = not dry_run or hands_reader is not None
        self._hold_watch = None    # the no-hands check's counts during a hold
        self._stop_note = ""       # why the session stopped, when it says more than "stopped"
        self._paused = False
        self._recording = False    # a recording part is running
        self._paused_recording = False
        self._waiting = False      # waiting for Next: no notes about lost hands
        self._redo_ok = False      # R does something now
        self._step_t0 = None       # the step's first event (ready or prompt), for R
        self._fb = {}              # feedback timers
        self._ring_path = None
        self._sides_read = 0.0     # when the side camera decision was last read

    # --- controls (any thread)
    def start(self):
        if self._thread:
            raise RuntimeError("the session has started already")
        self._thread = threading.Thread(target=self._run, name="handrec-session", daemon=True)
        self._thread.start()

    def _set(self, key, value):
        with self._lock:
            self._want[key] = value
        self._wake.set()

    def button_press(self):
        """The headset's button: Next while a step waits, pause during a countdown or hold
        (and auto mode's timed screens), resume while paused."""
        with self._lock:
            paused = self._want["pause"]
        state = self._status["state"]
        if paused:
            self.resume()
        elif self._waiting:
            self.next_step()
        elif state in ("countdown", "running", "intro", "between"):
            self.pause()
        else:
            return
        self._log("headset button (%s)" % ("resume" if paused else "next" if self._waiting else "pause"))

    def next_step(self):
        """Step mode: start the step that's waiting (its countdown)."""
        self._set("next", True)

    def redo(self):
        """Record a step again: the one running, or at a step's ready screen the one before."""
        self._set("redo", True)

    def pause(self):
        self._set("pause", True)

    def resume(self):
        self._set("pause", False)

    def skip(self):
        self._set("skip", True)

    def stop(self, wait=20.0):
        """Stop. The take in progress is kept as far as it got. Blocks until the session has
        written its files (up to wait seconds), unless called from the session thread."""
        self._set("stop", True)
        if self._thread and threading.current_thread() is not self._thread and wait:
            self._thread.join(wait)

    def join(self, timeout=None):
        if self._thread:
            self._thread.join(timeout)

    @property
    def state(self):
        return self._status["state"]

    # --- status and logging
    def _emit(self, force=True, **changes):
        self._status.update(changes)
        now = time.monotonic()
        if not force and now - self._last_emit < 0.25:
            return
        self._last_emit = now
        if self.on_status:
            try:
                self.on_status(dict(self._status, hands=dict(self._status["hands"])))
            except Exception:
                self._log("on_status: " + traceback.format_exc())

    def _log(self, text):
        if self._log_file:
            self._log_file.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))
            self._log_file.flush()

    def _event(self, event, **fields):
        """A line in the take's prompts.jsonl; returns its time."""
        t = mono_ns()
        if not self._take:
            return t
        line = {"t": t, "event": event}
        line.update(fields)
        self._take["prompts"].write(json.dumps(line) + "\n")
        self._take["prompts"].flush()
        return t

    # --- the run
    def _run(self):
        try:
            if self._preflight():
                return
            self._make_dir()
            self._emit(state="starting")
            self._setup()
            self._screen("starting", self.script.get("welcome") or {})
            for i, s in enumerate(self.plan):
                self._section(i, s)
            self._finish("done")
        except _Stop:
            self._finish("stopped")
        except _Fail as e:
            self._finish("error", str(e))
        except Exception as e:
            self._log(traceback.format_exc())
            self._finish("error", "%s: %s" % (type(e).__name__, e))

    def _preflight(self):
        """The camera check, before anything is made or started (camcheck.py): with the upper
        cameras off a session records nothing useful. True if it stopped the session (state
        "error", no session folder)."""
        if self.dry_run or self.ring or not self.check_cameras:
            return False
        r = self.camera_check_fn()
        self._status["camera"] = r
        text = camera_text(r)
        if not text:
            return False
        self._emit(state="error", error=text, prompt=text, camera=r)
        return True

    def _make_dir(self):
        sessions = os.path.join(self.base_dir, "sessions")
        os.makedirs(sessions, exist_ok=True)
        sid = time.strftime("%Y%m%d-%H%M%S")
        path, n = os.path.join(sessions, sid), 1
        while True:
            try:
                os.mkdir(path)
                break
            except FileExistsError:
                n += 1
                path = os.path.join(sessions, "%s-%d" % (sid, n))
        os.mkdir(os.path.join(path, "takes"))
        self.session_dir = path
        if self.seed is None:
            self.seed = session_seed(os.path.basename(path))
        # the same sections and steps, the sweeps shuffled for this session
        self.plan, self.skipped = build_plan(self.script, self.checklist, seed=self.seed, quick=self.quick)
        self._log_file = open(os.path.join(path, "session.log"), "a", buffering=1)
        self._log("session %s%s, script %s" % (os.path.basename(path), " (dry run)" if self.dry_run else "",
                                                self.script_path))
        for s in self.skipped:
            self._log("skipping %s: %s" % (s["section"], s["reason"]))
        cam = self._status.get("camera")
        if cam:
            self._log("camera check: %s" % cam.get("summary", cam.get("status")))
            for line in cam.get("evidence", []):
                self._log("  " + line)
        elif not self.check_cameras and not self.dry_run:
            self._log("camera check skipped (--ignore-cameras)")

    def _setup(self):
        ring_path = self.ring or os.path.join(self.hands_dir, "cam-ring")
        self._ring_path = ring_path
        if not self.dry_run:
            if not os.access(FT_HANDS, os.X_OK):
                raise _Fail("ft-hands isn't built: hands/build.sh")
            self._ensure_ring(ring_path)
            if not tracker_running():
                if self.start_processes:
                    self._start_tracker()
                else:
                    self._log("no tracking ft-hands found: feedback only if one publishes")
        lighting = ring_lighting(ring_path)
        cams = []
        try:
            ring = Ring(ring_path)
            cams = [{"name": c["name"], "width": c["width"], "height": c["height"]}
                    for c in ring.cams if not c["flags"] & FH_CAM_DARK and not c["name"].endswith("_dk")]
            ring.close()
        except (OSError, ValueError):
            pass
        removed = self._write_calibration() + self._write_device()
        self._session_json = {
            "schema": 1, "tool": "ft-handrec " + _git_describe(), "started": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
            "contributor": self.profile.get("contributor", ""),
            "lighting": lighting_record(self.lighting_choice, lighting),
            "checklist": self.checklist,
            "device": {"steamos": _os_version(), "steamvr": _steamvr_version(), "cameras": cams},
            "calibration_removed": removed,
            "script": {"version": self.script.get("version"), "sections": [s["id"] for s in self.plan],
                       "skipped": self.skipped},
            "mode": "auto" if self.auto else "step", "quick": self.quick,
            "shuffle": {"seed": self.seed, "sweeps": plan_record(self.plan)},
            "takes": [], "status": "recording", "sides": {"swapped": None}}
        self._read_sides(force=True)
        cam = self._status.get("camera")
        if cam:
            self._session_json["camera"] = {"status": cam.get("status"), "reason": cam.get("reason", "")}
            # which device each calibrated camera was (XRService's numbering) and whether XRService
            # ran the side cameras through the ISP (no colour module): to check the names later
            self._session_json["device"]["camera_map"] = cam.get("map") or {}
            self._session_json["device"]["isp"] = (cam.get("episode") or {}).get("isp")
        if self.dry_run:
            self._session_json["dry_run"] = True
        if self.speed != 1:
            self._session_json["speed"] = self.speed
        self._save_session()
        self._panel = Panel(dry=self.dry_run, log=self._log, out=self.print)
        if not self.dry_run:
            self._start_panel()
        self._start_button()
        self._panel.cmd("show")
        self._panel.cmd("paused off")
        for key, c in (("note", "note "), ("countdown", "countdown off"), ("hands", "hands off off"),
                       ("bar", "bar off"), ("target", "target off"), ("image", "image off"), ("where", "where off"),
                       ("big", "big "), ("action", "action "), ("rec", "rec off"), ("strip", "strip off")):
            self._panel.set(key, c)
        self._hints(action=False)

    def _start_button(self):
        """The headset button's reader, unless turned off (a dry run has none unless a test
        device is given). Without the device the session goes on: the window's keys work."""
        if not self.button or (self.dry_run and not self.button_device):
            return
        path = self.button_device or find_button(read_input_devices())
        if not path:
            self._log("headset button: no %s device with KEY_SELECT in %s" % (BUTTON_DEVICE, INPUT_DEVICES))
            return
        self._button = ButtonReader(path, self.button_press, log=self._log, debounce_s=BUTTON_DEBOUNCE_S).start()
        end = time.monotonic() + 0.5   # opened in a moment, or it isn't reachable
        while not self._button.ok and time.monotonic() < end:
            time.sleep(0.01)

    def _write_calibration(self):
        src = host_path("/persist/xrservice.json")
        if not src:
            self._log("no /persist/xrservice.json: no calibration.json")
            return []
        with open(src) as f:
            clean, removed = strip_calibration(json.load(f))
        write_json(os.path.join(self.session_dir, "calibration.json"), clean)
        return removed

    def _write_device(self):
        """device.json: the rig's pose in the CAD frame from /persist/device_config.json, only
        cv.cad_from_cal (Cam0 in CAD) and head (the head in CAD), in the shape the labeller reads
        (frame-hands train/label, as its cut.py writes it). The rest of that file names the unit
        (serial number, EDID). Returns what was removed, as "device.json:<path>"."""
        src = host_path("/persist/device_config.json")
        if not src:
            self._log("no /persist/device_config.json: no device.json")
            return []
        try:
            with open(src) as f:
                dev = json.load(f)
            picked = {"cv": {"cad_from_cal": dev["cv"]["cad_from_cal"]}, "head": dev["head"]}
        except (OSError, ValueError, KeyError, TypeError) as e:
            self._log("device_config.json unreadable (%s): no device.json" % e)
            return []
        clean, removed = strip_calibration(picked)
        write_json(os.path.join(self.session_dir, "device.json"), clean)
        return ["device.json:" + r for r in removed]

    def _save_session(self):
        if self._session_json is not None:
            write_json(os.path.join(self.session_dir, "session.json"), self._session_json)

    def _read_sides(self, force=False):
        """The tracking ft-hands' side camera decision (sides.py, read_live) into session.json's
        "sides": {"swapped", "decided_by", "state", "evidence", "decided_at"}. Later recording
        parts are named right (Recorder's swap); parts before it are renamed when read. Without
        a tracking ft-hands it stays undecided ("swapped": null): export then leaves the names,
        and the maintainer's check (hub_review check) tells."""
        now = time.monotonic()
        if self._session_json is None or (not force and now - self._sides_read < SIDES_READ_S):
            return
        self._sides_read = now
        live = sides.read_live(os.path.join(self.hands_dir, "sides.json"), self._ring_path)
        if not live or live.get("swapped") is None:
            return
        cur = self._session_json.get("sides") or {}
        swapped = bool(live["swapped"])
        if cur.get("swapped") is not None and bool(cur["swapped"]) == swapped:
            if live.get("state") != cur.get("state"):   # e.g. decided -> confirmed
                cur["state"] = live.get("state")
                self._save_session()
            return
        new = {"swapped": swapped, "decided_by": live.get("decided_by"), "state": live.get("state"),
               "evidence": live.get("evidence"), "decided_at": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
               "decided_ns": mono_ns()}
        if cur.get("swapped") is not None:
            new["reversed_from"] = cur
        self._session_json["sides"] = new
        self._save_session()
        self._log("side cameras: %s (%s, %s)" % ("SWAPPED" if swapped else "as named", new["decided_by"],
                                                  json.dumps(new["evidence"])))
        if new["state"] == "forced, disagrees":
            self._log("side cameras: HANDS_SWAP_SIDES in ~/.config/frametop.conf forces names the hands say are "
                      "backwards; the recording goes by the hands. Set HANDS_SWAP_SIDES=auto.")

    def _sides_swapped(self):
        """session.json's decision: True, False, or None (not known yet)."""
        return (self._session_json or {}).get("sides", {}).get("swapped")

    def _ensure_ring(self, path):
        if ring_alive(path):
            return
        if self.ring:
            raise _Fail("No frames in %s (start ft-ringplay first)" % path)
        if not self.start_processes:
            raise _Fail("ft-camd isn't running (and --no-start)")
        try:
            if start_camd(path, self._log, lambda: self._want["stop"]):
                self._units.append(CAMD_UNIT)
        except RuntimeError as e:
            raise _Fail(str(e))
        if self._want["stop"]:
            raise _Stop()

    def _start_tracker(self):
        up = os.path.join(REPO, "scripts", "container-up.sh")
        if os.access(up, os.X_OK):
            subprocess.run(host_command(up), capture_output=True, timeout=120)
        argv = [os.path.expanduser("~/.local/bin/distrobox"), "enter", "dev", "--", FT_HANDS,
                "--no-gestures", "--status", "0"]
        if self.ring:
            argv += ["--ring", self.ring]
        self._start_unit(HANDS_UNIT, "hand tracking for feedback", argv)

    def _start_unit(self, unit, what, argv):
        try:
            if start_unit(unit, what, argv, self._log):
                self._units.append(unit)
        except RuntimeError as e:
            raise _Fail(str(e))

    def _stop_units(self):
        for unit in reversed(self._units):
            stop_unit(unit)
            self._log("stopped %s" % unit)
        self._units = []

    def _start_panel(self):
        if self._panel.cmd("ping", reply=True):
            self._log("using the ft-handpanel that's running")
            return
        if not os.access(self.panel_bin, os.X_OK):
            raise _Fail("ft-handpanel isn't built: hands/rec/build.sh")
        self._panel_proc = subprocess.Popen([self.panel_bin, "--watch-stdin"], stdin=subprocess.PIPE,
                                            stdout=self._log_file, stderr=self._log_file)
        end = time.monotonic() + 15
        while time.monotonic() < end:
            if self._panel.cmd("ping", reply=True, timeout=0.2):
                return
            if self._panel_proc.poll() is not None:
                raise _Fail("ft-handpanel exited (is SteamVR running?): see session.log")
            time.sleep(0.1)
        raise _Fail("ft-handpanel doesn't answer")

    def _stop_panel(self):
        if self._panel:
            self._panel.cmd("hide")
        if self._panel_proc:
            try:
                self._panel_proc.stdin.close()
                self._panel_proc.wait(3)
            except (OSError, subprocess.TimeoutExpired):
                self._panel_proc.terminate()
                try:
                    self._panel_proc.wait(3)
                except subprocess.TimeoutExpired:
                    self._panel_proc.kill()
            self._panel_proc = None
        if self._panel:
            self._panel.close()

    def _finish(self, state, error=""):
        if self._take:
            self._end_take({"done": "complete", "stopped": "stopped"}.get(state, "stopped"))
        if self._session_json is not None:
            self._session_json["status"] = state
            self._session_json["ended"] = time.strftime("%Y-%m-%dT%H:%M:%S%z")
            if error:
                self._session_json["error"] = error
            if self._stop_note and state == "stopped":
                self._session_json["stop_reason"] = self._stop_note
            self._save_session()
        self._log("%s%s" % (state, ": " + error if error else ""))
        if self._stop_note and state == "stopped":
            self._log(self._stop_note)
        self._stop_units()
        if self._button:
            self._button.stop()
            self._button = None
        if self._panel:
            screen = {"done": self.script.get("done") or {}, "stopped": self.script.get("stopped") or {},
                      "error": {"title": "Something went wrong", "text": "See the Hand recorder window.",
                                "seconds": 4}}[state]
            try:
                self._panel.cmd("paused off")
                for key, c in (("big", "big "), ("action", "action "), ("rec", "rec off"), ("keys", "keys "),
                               ("bar", "bar off"), ("target", "target off"), ("strip", "strip off")):
                    self._panel.set(key, c)
                self._screen(state, screen, controls=False)
            except (_Stop, _Skip, _Fail):
                pass
        self._stop_panel()
        if self._log_file:
            self._log_file.close()
            self._log_file = None
        screen = {"done": self.script.get("done"), "stopped": self.script.get("stopped")}.get(state) or {}
        self._emit(state=state, error=error, seconds_left=0.0, note="", take=None, section="",
                   prompt=error or (self._stop_note if state == "stopped" else "") or screen.get("text", "").replace("|", "\n"),
                   hands={"left": None, "right": None}, nohands=False,
                   waiting=False, countdown=0, big="", can_redo=False, image="", image_mode="", caption="",
                   position="", distance="", strip=[], cue=-1)

    # --- the timing loop
    def _controls(self):
        """Handle a pause (blocking until resumed), skip, stop and redo (only where _redo_ok:
        else it's dropped). True if a pause happened."""
        with self._lock:
            want = dict(self._want)
            self._want["skip"] = self._want["redo"] = False
        if want["stop"]:
            raise _Stop()
        if want["skip"]:
            raise _Skip()
        if want["redo"] and self._redo_ok:
            raise _Redo()
        if not want["pause"]:
            return False
        self._pause()
        outcome = None
        while outcome is None:
            self._wake.wait(0.2)
            self._wake.clear()
            with self._lock:
                want = dict(self._want)
                self._want["skip"] = self._want["redo"] = False
                redo = want["redo"] and self._redo_ok
                if want["skip"] or redo:
                    self._want["pause"] = False
            if want["stop"]:
                outcome = _Stop
            elif want["skip"]:
                outcome = _Skip
            elif redo:
                outcome = _Redo
            elif not want["pause"]:
                outcome = True
        self._unpause(record=outcome is True)
        if outcome is not True:
            raise outcome()
        return True

    def _pause(self):
        self._paused = True
        self._state_before = self._status["state"]
        self._paused_recording = self._recording
        if self._recording:
            self._stop_recording()
            self._event("pause")
        self._panel.cmd("paused on")
        self._panel.set("note", "note " + RESUME_HINT)
        self._emit(state="paused", note=RESUME_HINT)
        self._log("paused")

    def _unpause(self, record=True):
        self._paused = False
        self._panel.cmd("paused off")
        self._panel.set("note", "note ")
        self._fb.clear()
        with self._lock:
            self._want["next"] = False   # a Next pressed while paused doesn't count
        if self._take and self._paused_recording and record:
            self._event("resume")
            self._start_recording()
        self._emit(state=self._state_before, note="")
        self._log("resumed")

    def _wait(self, seconds, tick=None, countdown=True):
        """Let `seconds` (script time) pass, handling the controls and feedback. tick(dt) runs
        every step with the script time passed; it ends the wait early by returning True.
        Returns True if tick ended it, else False."""
        left = float(seconds)
        last = time.monotonic()
        last_cd = -1.0
        while True:
            if self._controls():
                last = time.monotonic()
            now = time.monotonic()
            dt = (now - last) * self.speed
            last = now
            left -= dt
            self._feedback()
            if tick and tick(dt):
                return True
            if countdown and seconds > 0 and (now - last_cd >= 0.25 or left <= 0):
                last_cd = now
                self._panel.set("countdown", "countdown %.2f" % max(0.0, min(1.0, left / seconds)))
            self._emit(force=False, seconds_left=round(max(left, 0.0), 1))
            if left <= 0:
                return False
            self._wake.wait(TICK_S)
            self._wake.clear()

    # --- feedback
    def _feedback(self):
        now = time.monotonic()
        fb = self._fb
        if self._panel_proc and self._panel_proc.poll() is not None:
            raise _Fail("The headset panel closed (did SteamVR quit?)")
        if self._recorder and self._recorder.exited():
            raise _Fail("The recording stopped by itself: see session.log")
        if not self.dry_run and now - fb.get("disk", 0) > 5:
            fb["disk"] = now
            if shutil.disk_usage(self.session_dir).free < MIN_FREE:
                raise _Fail("The disk is nearly full: the session stopped")
        if not self.dry_run:
            self._read_sides()
        live = self._hands_file.read() if self._read_hands else None
        self._live = live
        w = self._hold_watch
        if w is not None:
            w["reads"] += 1
            if live is not None:
                w["published"] += 1
                if live["left"] or live["right"]:
                    w["seen"] += 1
        p = self._prompt or {}
        asked = p.get("hands", "")
        # While a step waits for Next the chips show what's seen, with no notes yet.
        seen = {s: (bool(live[s]) if live else None) for s in ("left", "right")}
        # the chips: the asked-for hands, seen or lost, while the tracker publishes
        chips = []
        for side in ("left", "right"):
            show = live is not None and (asked in (side, "both", "any"))
            chips.append(("seen" if seen[side] else "lost") if show else "off")
        self._panel.set("hands", "hands %s %s" % tuple(chips))
        # a note when an asked-for hand stays lost, or a hand shows when none is wanted
        notes = []
        if live is not None and asked and not self._waiting:
            if asked == "none":
                missing = [] if not (seen["left"] or seen["right"]) else ["shown"]
            elif asked == "any":
                missing = [] if (seen["left"] or seen["right"]) else ["any"]
            else:
                missing = [s for s in ("left", "right") if asked in (s, "both") and not seen[s]]
            key = ",".join(missing)
            if key != fb.get("lost_key"):
                fb["lost_key"], fb["lost_since"] = key, now
            if missing and now - fb["lost_since"] > LOST_S:
                if missing == ["shown"]:
                    notes.append("I can see a hand: keep them out of view")
                elif missing == ["any"] or len(missing) == 2:
                    notes.append("I can't see your hands: bring them into view")
                else:
                    notes.append("I can't see your %s hand: bring it into view" % missing[0])
        else:
            fb.pop("lost_key", None)
        notes = ([] if self._waiting else self._controller_feedback(now, p)) + notes
        note = notes[0] if notes else ""
        if not self._paused:
            self._panel.set("note", "note " + note)
        hands = {"left": seen["left"], "right": seen["right"]}
        if note != self._status["note"] or hands != self._status["hands"]:
            self._emit(note=note, hands=hands)
        if self._take and self._recording and live is not None and now - fb.get("logged", 0) >= 0.5:
            fb["logged"] = now
            self._event("feedback", left=seen["left"], right=seen["right"],
                        palm_m=[round(live[s]["palm_m"], 4) if live[s] else None for s in ("left", "right")])

    def _controller_feedback(self, now, p):
        """Sections with controllers: `devices` once a second; a result other than 200 for
        more than a second gets a note, and each change goes into prompts.jsonl."""
        sides = p.get("controllers") or []
        fb = self._fb
        if not sides:
            fb.pop("ctl", None)
            return []
        ctl = fb.setdefault("ctl", {"polled": 0, "r": {}, "bad_since": {}, "lost": []})
        if now - ctl["polled"] >= 1.0:
            ctl["polled"] = now
            reply = self._panel.cmd("devices", reply=True, timeout=0.3) or ""
            tok = reply.split()
            vals = dict(zip(tok[1::2], tok[2::2]))
            if tok[:1] == ["ok"] and "hmd" in vals:
                ctl["r"] = {s: vals.get(s, "-") for s in ("left", "right")}
            for s in sides:
                r = ctl["r"].get(s)
                if r is None or r == "200":
                    ctl["bad_since"].pop(s, None)
                else:
                    ctl["bad_since"].setdefault(s, now)
        lost = sorted(s for s, t in ctl["bad_since"].items() if s in sides and now - t > CONTROLLER_LOST_S)
        if lost != ctl["lost"]:
            ctl["lost"] = lost
            self._event("feedback", controller={s: (None if ctl["r"].get(s) in (None, "-") else int(ctl["r"][s]))
                                                for s in ("left", "right")}, controller_lost=lost)
        notes = []
        for s in lost:
            if ctl["r"].get(s) == "-":
                notes.append("I can't find the %s controller: is it on?" % s)
            else:
                notes.append("The %s controller lost tracking: turn your palm slightly toward you" % s)
        return notes

    # --- sections and takes
    def _show(self, title=None, step=None, text=None):
        if title is not None:
            self._panel.set("title", "title " + clean_text(title))
        if step is not None:
            self._panel.set("step", "step " + clean_text(step))
        if text is not None:
            self._panel.set("text", "text " + clean_text(text))

    def _view(self, p=None):
        """The prompt's picture and where-to diagram, on the panel and in the status (sent with
        the next _emit); none without a prompt."""
        p = p or {}
        path, mode, caption = ("", "", "") if p.get("cues") else pose_view(self._poses, p)
        self._strip(p)
        pos, dist = p.get("position") or "", p.get("distance") or ""
        self._panel.set("image", "image %s%s" % (path, " " + mode if mode else "") if path else "image off")
        self._panel.set("where", "where %s %s" % (clean_text(pos) or "-", clean_text(dist) or "-")
                        if pos or dist else "where off")
        self._status.update(image=path, image_mode=mode, caption=caption, position=pos, distance=dist)

    def _cue_name(self, pose):
        return clean_text((self.script.get("cue_names") or {}).get(pose) or pose.replace("-", " ").capitalize())

    def _strip(self, p):
        """A sweep's strip of pictures, the cue's highlighted (panel "strip", status strip and
        cue), or none. One hand per picture, as both hands make the same shape: a left hand's
        flipped, the rest as drawn."""
        poses = list(dict.fromkeys(p.get("cues") or []))   # each once, in the step's order
        if not poses:
            self._panel.set("strip", "strip off")
            self._status.update(strip=[], cue=-1)
            return
        items = []
        for pose in poses:
            path, mode, _ = pose_view(self._poses, {"pose": pose, "hands": p.get("hands")})
            items.append({"image": path, "mode": "" if mode == "both" else mode, "label": self._cue_name(pose)})
        cue = poses.index(p["pose"]) if p.get("pose") in poses else -1
        self._panel.set("strip", "strip %d %s" % (cue, ";".join(
            "%s|%s|%s" % (e["image"] or "-", e["mode"] or "-", e["label"].replace("|", "/").replace(";", ","))
            for e in items)))
        self._status.update(strip=items, cue=cue)

    def _hints(self, action=True):
        """The Next hint (with action) and the key line for what's there now: the headset button
        leads when no mouse is connected. Looked at again for each step, so a mouse plugged in counts."""
        button = bool(self._button and self._button.ok)
        mouse = mouse_connected(self.input_devices)
        text = (READY_BUTTON_MOUSE if mouse else READY_BUTTON) if button else READY_TEXT
        keys = (KEYS_AUTO_BUTTON if self.auto else KEYS_STEP_BUTTON) if button else (KEYS_AUTO if self.auto else KEYS_STEP)
        if action:
            self._panel.set("action", "action " + text)
        self._panel.set("keys", "keys " + keys)
        self._status.update(ready_text=text, button=button, mouse=mouse)

    def _await_next(self):
        """Step mode: wait for Next (or the next_after test hook), handling the controls and the
        hands chips. Nothing records meanwhile."""
        with self._lock:
            self._want["next"] = False   # one pressed during the hold doesn't skip this
        self._waiting = True
        self._hints()
        self._emit(waiting=True, seconds_left=0.0)
        t0 = time.monotonic()
        try:
            while True:
                self._controls()
                self._feedback()
                with self._lock:
                    go, self._want["next"] = self._want["next"], False
                if go or (self.next_after is not None and time.monotonic() - t0 >= self.next_after):
                    break
                self._wake.wait(TICK_S)
                self._wake.clear()
        finally:
            self._waiting = False
            self._panel.set("action", "action ")
        self._emit(waiting=False)

    def _screen(self, state, screen, controls=True):
        """A screen of its own (welcome, done): title, text, a few seconds, or in step mode
        until Next. controls=False: the session's end, which reports its state once all is done."""
        if not screen:
            return
        self._prompt = None
        self._show(screen.get("title", ""), "", screen.get("text", ""))
        self._view()
        self._panel.set("countdown", "countdown off")
        if controls:
            self._emit(state=state, title=screen.get("title", ""), prompt=screen.get("text", "").replace("|", "\n"),
                       section="", seconds_left=float(screen.get("seconds", 0)))
            try:
                if self.auto:
                    self._wait(screen.get("seconds", 0), countdown=False)
                else:
                    self._await_next()
            except _Skip:
                pass
        else:
            end = time.monotonic() + screen.get("seconds", 0) / self.speed
            while time.monotonic() < end and not self._want["stop"]:
                time.sleep(TICK_S)

    def _section(self, i, s):
        step = "Section %d of %d" % (i + 1, len(self.plan))
        status = {"section": s["id"], "title": s["title"], "section_index": i + 1, "section_count": len(self.plan),
                  "step_index": 0, "step_count": section_steps(s) if not self.auto else 0, "can_redo": False}
        try:
            before = s.get("before")
            self._prompt = None
            self._view()
            if self.auto:
                if before or i > 0:
                    text = before["text"] if before else "Next: %s" % s["title"]
                    secs = before.get("seconds", 10) if before else self.script.get("between_s", 3)
                    self._show("Get ready" if before else s["title"], step, text)
                    self._emit(state="between", prompt=text.replace("|", "\n"), take=None, **status)
                    self._wait(secs)
                self._start_take(i, s)
                self._status.update(state="intro", take=self._take["id"], **status)   # sent with the intro
                self._run_prompt(s, {"id": s["id"] + "/intro", "text": s.get("intro", ""), "seconds": s["intro_s"],
                                     "hands": "", "pose": "", "distance": "", "position": "", "object": "",
                                     "controller": False, "controllers": []}, step, intro=True)
            else:
                # One screen before the section: what to get ready, and the intro. The take
                # starts with the first step's countdown, so a section skipped here leaves none.
                text = "|".join(t for t in ((before or {}).get("text", ""), s.get("intro", "")) if t)
                self._show(s["title"], step, text)
                self._panel.set("countdown", "countdown off")
                self._emit(state="intro", prompt=text.replace("|", "\n"), take=None, seconds_left=0.0, **status)
                self._await_next()
            self._status["state"] = "running"   # sent with the first prompt
            self._steps(i, s, step)
            if self._take:
                self._end_take("complete")
        except _Skip:
            self._log("skipped %s" % s["id"])
            if self._take:
                self._end_take("skipped")
        except _Stop:
            raise
        finally:
            self._redo_ok = False
            self._status["big"] = ""
            for key, c in (("bar", "bar off"), ("target", "target off"), ("big", "big "), ("action", "action ")):
                self._panel.set(key, c)

    def _start_take(self, i, s, record=True):
        n = len(self._session_json["takes"]) + 1
        take_id = "%02d-%s" % (n, s["id"])
        d = os.path.join(self.session_dir, "takes", take_id)
        os.makedirs(d)
        self._take = {"id": take_id, "dir": d, "section": s, "part": 0,
                      "prompts": open(os.path.join(d, "prompts.jsonl"), "a", buffering=1),
                      "json": {"section": s["id"], "title": s["title"], "started_ns": mono_ns(), "ended_ns": None,
                               "status": "stopped", "deleted": [], "notes": ""}}
        write_json(os.path.join(d, "take.json"), self._take["json"])
        self._session_json["takes"].append(take_id)
        self._save_session()
        self._event("take", section=s["id"], take=take_id)
        if record:
            self._start_recording()
        self._log("take %s" % take_id)
        self._emit(force=False, take=take_id)

    def _start_recording(self):
        t = self._take
        t["part"] += 1
        t["json"].setdefault("clock", []).append(clock_sample())
        if not self.dry_run:
            # a safety net only: the session ends the recording itself
            remaining = section_seconds(t["section"], worst=True, auto=self.auto) / self.speed
            self._recorder = Recorder(t["dir"], t["part"], remaining * 1.5 + 60, self.ring, self._log_file,
                                      swap=self._sides_swapped())
        self._recording = True
        self._panel.cmd("poses start " + os.path.join(t["dir"], "poses.jsonl"))
        self._panel.set("rec", "rec on")

    def _stop_recording(self):
        if not self._recording:
            return
        self._recording = False
        self._panel.set("rec", "rec off")
        self._panel.cmd("poses stop", reply=not self.dry_run, timeout=1.0)
        if self._recorder:
            rec, self._recorder = self._recorder, None
            code = rec.stop()
            self._log("recording part %d ended (%s)" % (rec.part, code))
            if self._take:   # how this part's side cameras are named (sides.py)
                self._take["json"].setdefault("parts", {})[rec.file] = {"names_swapped": rec.names_swapped}
                self._take["json"].setdefault("clock", []).append(clock_sample())
                write_json(os.path.join(self._take["dir"], "take.json"), self._take["json"])

    def _end_take(self, status):
        t = self._take
        try:
            self._stop_recording()
        finally:
            self._event("end", status=status)
            self._take = None
            t["prompts"].close()
            t["json"]["ended_ns"] = mono_ns()
            t["json"]["status"] = status
            write_json(os.path.join(t["dir"], "take.json"), t["json"])
            self._log("take %s %s" % (t["id"], status))

    def _prompt_event(self, p):
        extra = {"controllers": p["controllers"]} if p["controllers"] else {}
        if p.get("cue"):   # a sweep's cue: the pose highlighted from now, within the step
            extra.update(cue=True, step=p["step"])
        return self._event("prompt", id=p["id"], text=p["text"], hands=p["hands"], pose=p["pose"],
                           distance=p["distance"], position=p["position"], object=p["object"],
                           controller=bool(p["controller"]), **extra)

    def _begin_prompt(self, s, p, step, state="running"):
        self._prompt = p
        self._fb.pop("lost_key", None)
        self._show(s["title"], step, p["text"])
        self._view(p)
        t = self._prompt_event(p)
        if self._step_t0 is None:
            self._step_t0 = t
        self._emit(state=state, prompt=p["text"].replace("|", "\n"), seconds_left=float(p["seconds"]), countdown=0)
        self._log("  %s" % p["id"])

    def _run_prompt(self, s, p, step, intro=False):
        if intro and not p["text"]:
            return
        self._begin_prompt(s, p, step, state="intro" if intro else "running")
        self._wait(p["seconds"])

    # --- steps: a section's prompts, bar heights and targets
    def _step_list(self, s):
        """[{"kind": "prompt"|"sweep"|"bar"|"target", "p": prompt, "ready": waits for Next in step mode, ...}]."""
        out = []
        defaults = s.get("defaults") or {}
        if s["kind"] == "targets":
            for k, pt in enumerate(s["targets"]):
                p = {"id": "%s/%d" % (s["id"], k + 1), "text": s.get("text", ""), "seconds": float(s.get("timeout_s", 8)),
                     "hands": defaults.get("hands", "any"), "pose": defaults.get("pose", "point"), "distance": "",
                     "position": "", "object": "", "controller": False, "controllers": []}
                out.append({"kind": "target", "p": p, "pt": pt, "ready": k == 0})
        elif s["kind"] == "bar":
            lead = float(s.get("lead_s", 3)) if self.auto else 0.0   # step mode: the countdown shows the bar at near
            for h in s["heights"]:
                p = {"id": "%s/%s" % (s["id"], h["id"]), "text": h["text"], "hands": defaults.get("hands", "both"),
                     "pose": defaults.get("pose", "open"), "distance": "", "position": h["id"], "object": "",
                     "picture": h.get("picture", defaults.get("picture", "")),
                     "controller": bool(defaults.get("controller", False)),
                     "controllers": list(defaults.get("controllers") or []),
                     "seconds": lead + h["reps"] * float(s.get("period_s", 6))}
                out.append({"kind": "bar", "p": p, "h": h, "lead": lead, "ready": True})
        out += [{"kind": "sweep" if p.get("cues") else "prompt", "p": p, "ready": True} for p in s["prompts"]]
        return out

    def _steps(self, i, s, label):
        """Run a section's steps. Step mode: each that's "ready" (and each to do again) shows
        first and waits for Next; then the recorded countdown; the recording stops after a
        step unless the next one follows straight on (the targets after the first). R: the
        step running starts again; at a ready screen, the step before goes again. Either way
        the range done before is marked with a "redo" event."""
        steps = self._step_list(s)
        done = []      # the steps finished in this take: (index, id, from_ns, to_ns), for R
        again = set()  # steps to do again: they wait for Next too
        retry = False  # after the no-hands stop: Next was pressed there, go straight to the countdown
        k = 0
        while k < len(steps):
            st, p = steps[k], steps[k]["p"]
            where = label if self.auto else "%s · step %d of %d" % (label, k + 1, len(steps))
            self._status.update(step_index=k + 1, step_count=len(steps))
            started, self._step_t0 = None, None
            try:
                if not self.auto and (st["ready"] or k in again):
                    self._redo_ok = bool(done)
                    self._ready(s, st, where, can_redo=bool(done))
                    if not retry:
                        self._await_next()
                    retry = False
                    self._redo_ok = True
                    started = self._countdown(i, s, st)
                else:
                    retry = False
                    self._redo_ok = True
                    self._emit(force=False, can_redo=True)
                check = self._read_hands and s["id"] == HANDS_CHECK_SECTION and k == 0 and st["kind"] in ("prompt", "sweep")
                self._hold_watch = {"reads": 0, "published": 0, "seen": 0} if check else None
                try:
                    self._run_step(s, st, where)
                finally:
                    watch, self._hold_watch = self._hold_watch, None
                if check and self._no_hands_seen(p, watch):
                    self._no_hands(s, st, where, started if started is not None else self._step_t0, watch)
                    again.add(k)
                    retry = True
                    continue
                done.append((k, p["id"], started if started is not None else self._step_t0, mono_ns()))
                if not self.auto and (k + 1 == len(steps) or steps[k + 1]["ready"] or k + 1 in again):
                    self._hold_end()
                again.discard(k)
                k += 1
            except _Redo:
                self._redo_ok = False
                from_ns = started if started is not None else self._step_t0
                self._panel.set("target", "target off")
                self._panel.set("bar", "bar off")
                if from_ns is not None:   # R during the step: it starts again
                    self._event("redo", id=p["id"], **{"from": from_ns, "to": mono_ns()})
                    self._log("    redo %s" % p["id"])
                    if not self.auto:
                        self._hold_end()
                    elif self._take and not self._recording:   # R ended a pause: record again
                        self._event("resume")
                        self._start_recording()
                    again.add(k)
                elif done:                # R at its ready screen: the step before goes again
                    k, pid, a, b = done.pop()
                    self._event("redo", id=pid, **{"from": a, "to": b})
                    self._log("    redo %s" % pid)
                    again.add(k)

    def _no_hands_seen(self, p, w):
        """The no-hands check's verdict on a hold (logged either way): True if the tracker
        published through it and never saw a hand."""
        enough = w["reads"] >= HANDS_CHECK_MIN_READS and w["published"] >= HANDS_CHECK_PUBLISHED * w["reads"]
        if not enough:
            self._log("    hands check %s: the tracker published in %d of %d reads: can't tell"
                      % (p["id"], w["published"], w["reads"]))
            return False
        self._log("    hands check %s: a hand in %d of %d reads (%d published)"
                  % (p["id"], w["seen"], w["reads"], w["published"]))
        return w["seen"] == 0

    def _no_hands(self, s, st, where, from_ns, w):
        """The first step saw no hands at all: stop it (its range marked as redone, so it gets
        no labels), run the camera check, say so, and wait. Next or R tries the step again; Stop
        (Esc) ends the session with the camera check's result; S skips the section."""
        p = st["p"]
        if not self.auto:
            self._hold_end()
        elif self._recording:
            self._stop_recording()
            self._event("pause")
        self._event("nohands", id=p["id"], reads=w["reads"], published=w["published"])
        if from_ns is not None:
            self._event("redo", id=p["id"], **{"from": from_ns, "to": mono_ns()})
        cam = self.camera_check_fn() if self.check_cameras else None
        cam_text = camera_text(cam)
        summary = (cam or {}).get("summary", "not run")
        self._log("    no hands seen in %s: asking to try again or stop; camera check: %s" % (p["id"], summary))
        for line in (cam or {}).get("evidence", []):
            self._log("      " + line)
        text = "%s|%s|%s" % (NO_HANDS_TEXT, cam_text or "The camera check found nothing wrong (%s)." % summary,
                             NO_HANDS_RETRY)
        self._stop_note = "Stopped: no hands were seen in the first step. Camera check: %s." % summary
        if cam_text:
            self._stop_note += " " + cam_text
        self._prompt = None
        self._view()
        self._show(NO_HANDS_TITLE, where, text)
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("bar", "bar off"), ("target", "target off"),
                       ("note", "note "), ("hands", "hands off off")):
            self._panel.set(key, c)
        self._emit(state="nohands", prompt=text.replace("|", "\n"), camera=cam, nohands=True, can_redo=True,
                   big="", countdown=0, seconds_left=0.0)
        self._redo_ok = True
        try:
            self._await_next()
        except _Redo:
            pass   # R here is the same as Next: try again
        except _Skip:
            self._stop_note = ""
            self._emit(nohands=False)
            raise
        self._stop_note = ""
        self._log("    trying %s again" % p["id"])
        self._emit(nohands=False)
        if self.auto and self._take and not self._recording:
            self._event("resume")
            self._start_recording()

    def _ready(self, s, st, where, can_redo):
        """Step mode: show the step (text, picture, diagram) with "Ready?"."""
        p = st["p"]
        self._prompt = p   # the chips show which hands are seen while the person gets ready
        self._fb.pop("lost_key", None)
        self._show(s["title"], where, p["text"])
        self._view(p)
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("bar", "bar off"), ("target", "target off")):
            self._panel.set(key, c)
        self._emit(state="ready", prompt=p["text"].replace("|", "\n"), seconds_left=float(p["seconds"]),
                   can_redo=can_redo, countdown=0, big="")

    def _countdown(self, i, s, st):
        """Step mode: start recording and count 3-2-1 (logged as a "ready" event, so labels
        cover only the hold), then make sure the recording has its first set. Returns the
        ready event's time."""
        p = st["p"]
        if not self._take:
            self._start_take(i, s, record=False)
        t = self._event("ready", id=p["id"], seconds=COUNTDOWN_S)
        self._step_t0 = t
        self._start_recording()
        tick = None
        if st["kind"] == "bar":   # the bar at near, where the sweep starts
            labels = "%s|%s" % (s.get("near_label", "Near"), s.get("far_label", "Far"))
            near, far = float(s.get("near_m", 0.2)), float(s.get("far_m", 0.6))

            def tick(dt):
                self._panel.set("bar", "bar 0.000 %.3f %s" % (self._palm_share(near, far), labels))
                return False
        for n in range(COUNTDOWN_S, 0, -1):
            self._panel.set("big", "big %d" % n)
            self._emit(state="countdown", countdown=n, big=str(n), can_redo=True)
            self._wait(1.0, tick, countdown=False)
        self._first_set()
        self._panel.set("big", "big " + s.get("go", "Go"))
        self._status["big"] = s.get("go", "Go")   # sent with the prompt
        return t

    def _first_set(self):
        """The hold starts once its recording has a set. ft-hands --record-only writes its first
        within about 30 ms of starting, so the countdown covers it; this is a safety net."""
        rec = self._recorder
        if not rec:
            return
        end = time.monotonic() + FIRST_SET_S
        while not rec.has_data():
            if rec.exited():
                raise _Fail("The recording stopped by itself: see session.log")
            if self._want["stop"]:
                raise _Stop()
            if time.monotonic() > end:
                self._log("recording part %d: no set yet after the countdown" % rec.part)
                return
            time.sleep(0.02)

    def _hold_end(self):
        """Step mode: the step is over. A "wait" event ends its labels and the recording stops
        until the next countdown."""
        self._event("wait")
        self._stop_recording()
        self._status["big"] = ""
        for key, c in (("big", "big "), ("countdown", "countdown off"), ("bar", "bar off"), ("target", "target off")):
            self._panel.set(key, c)

    def _run_step(self, s, st, where):
        if st["kind"] != "bar":
            self._panel.set("bar", "bar off")
        if st["kind"] == "target":
            self._target(s, st, where)
        elif st["kind"] == "bar":
            self._bar(s, st, where)
        elif st["kind"] == "sweep":
            self._sweep(s, st, where)
        else:
            self._run_prompt(s, st["p"], where)

    def _sweep(self, s, st, where):
        """A sweep: the strip's pictures highlighted in turn, one every cue_s, while the hands
        keep moving. Each cue is a prompt event of its own, with that pose, "cue": true and the
        step's id, so the timeline tags each pose roughly; the time-left bar covers the step."""
        p = st["p"]
        cues, cue_s = p["cues"], p["cue_s"]
        seen = {}

        def cue_prompt(j):
            pose = cues[j]
            seen[pose] = seen.get(pose, 0) + 1
            return dict(p, id="%s/%s%s" % (p["id"], pose, "#%d" % seen[pose] if seen[pose] > 1 else ""),
                        pose=pose, cue=True, step=p["id"])

        self._begin_prompt(s, cue_prompt(0), where)
        state = {"t": 0.0, "j": 0}

        def tick(dt):
            state["t"] += dt
            j = min(len(cues) - 1, int(state["t"] / cue_s + 1e-9))
            if j != state["j"]:
                state["j"] = j
                cp = cue_prompt(j)
                self._prompt = cp
                self._strip(cp)
                self._prompt_event(cp)
                self._emit()
                self._log("    cue %s" % cp["id"])
            return False

        self._wait(p["seconds"], tick)

    def _target(self, s, st, where):
        hold_s, timeout_s = float(s.get("hold_s", 1.0)), float(s.get("timeout_s", 8))
        p, pt = st["p"], st["pt"]
        self._begin_prompt(s, p, where)
        xyz = "%.3f %.3f %.3f" % tuple(pt)
        reply = self._panel.cmd("target %s show" % xyz, reply=True) or ""
        self._panel.sent["target"] = "target %s show" % xyz
        tok = reply.split()
        room = [float(v) for v in tok[1:4]] if tok[:1] == ["ok"] and len(tok) >= 4 else None
        target = {"id": p["id"], "head": list(pt), "room": room}
        self._event("target", state="show", **target)
        state = {"hold": 0.0, "sent": 0.0, "holding": False}

        def tick(dt):
            d = self._tip_distance(pt, room)
            if d is not None and d <= TOUCH_M:
                if not state["holding"]:
                    state["holding"] = True
                    self._event("target", state="hold", **target)
                state["hold"] += dt
            elif state["holding"]:
                state["holding"], state["hold"] = False, 0.0
                self._panel.set("target", "target %s show" % xyz)
            if state["holding"]:
                frac = min(1.0, state["hold"] / hold_s)
                if time.monotonic() - state["sent"] >= 0.1 or frac >= 1:
                    state["sent"] = time.monotonic()
                    self._panel.set("target", "target %s hold %.2f" % (xyz, frac))
            return state["hold"] >= hold_s

        done = self._wait(timeout_s, tick)
        result = "done" if done else "timeout"
        if done:
            self._panel.set("target", "target %s done" % xyz)
        self._event("target", state=result, **target)
        self._log("    %s %s" % (p["id"], result))
        if done:
            self._wait(0.6, countdown=False)
        self._panel.set("target", "target off")

    def _tip_distance(self, pt, room):
        """The nearest seen index tip's distance from the target: in the room (with the head's
        pose now) when the panel placed the target there, else in the head frame."""
        live = self._live
        if not live:
            return None
        tips = [live[s]["tip"] for s in ("left", "right") if live[s]]
        if not tips:
            return None
        ref = pt
        if room is not None:
            reply = (self._panel.cmd("head", reply=True, timeout=0.1) or "").split()
            if reply[:1] == ["ok"] and len(reply) == 13:
                m = [float(v) for v in reply[1:]]
                tips = [[m[4 * r] * t[0] + m[4 * r + 1] * t[1] + m[4 * r + 2] * t[2] + m[4 * r + 3] for r in range(3)]
                        for t in tips]
                ref = room
        return min(math.dist(t, ref) for t in tips)

    def _palm_share(self, near, far):
        live = self._live
        if not live:
            return -1.0
        ds = [live[s]["palm_m"] for s in ("left", "right") if live[s]]
        if not ds:
            return -1.0
        return max(0.0, min(1.0, (sum(ds) / len(ds) - near) / (far - near)))

    def _bar(self, s, st, where):
        """One height of the push out and back: the target sweeps near to far and back."""
        near, far = float(s.get("near_m", 0.2)), float(s.get("far_m", 0.6))
        period, lead = float(s.get("period_s", 6)), st["lead"]
        labels = "%s|%s" % (s.get("near_label", "Near"), s.get("far_label", "Far"))
        p = st["p"]
        self._begin_prompt(s, p, where)
        state = {"t": 0.0, "sent": 0.0}

        def tick(dt):
            state["t"] += dt
            sweep = state["t"] - lead
            target = 0.0 if sweep <= 0 else 1 - abs(1 - 2 * ((sweep % period) / period))
            if time.monotonic() - state["sent"] >= 0.1:
                state["sent"] = time.monotonic()
                cur = self._palm_share(near, far)
                self._panel.set("bar", "bar %.3f %.3f %s" % (target, cur, labels))
                if sweep > 0:
                    self._event("bar", target=round(target, 3), current=None if cur < 0 else round(cur, 3))
            return False

        self._wait(p["seconds"], tick)


# ------------------------------------------------------------------------------------------
# The command line

def main():
    ap = argparse.ArgumentParser(description="Run a hand recording session (the hand recorder's session runner).")
    ap.add_argument("--dry-run", action="store_true", help="run no processes; print the panel commands")
    ap.add_argument("--speed", type=float, default=1.0, help="run the script this many times faster")
    ap.add_argument("--ring", help="read frames from this ring (ft-ringplay's) instead of ft-camd's")
    ap.add_argument("--no-start", action="store_true", help="start no ft-camd or tracking ft-hands")
    ap.add_argument("--base", help="where sessions go (default %s; a temporary folder with --dry-run)" % BASE_DIR)
    ap.add_argument("--objects", default="", help="ticked objects, comma-separated (unknown names are your own)")
    ap.add_argument("--controllers", action="store_true", help="controllers with the straps")
    ap.add_argument("--lighting", default="auto", choices=("auto", "dim", "room", "daylight"),
                    help="this round's light (auto: indoor or daylight, from the cameras)")
    ap.add_argument("--script", default=SCRIPT_PATH)
    ap.add_argument("--panel", help="the panel program (default hands/rec/build/ft-handpanel)")
    ap.add_argument("--hands-dir", help="where the hands file is (default /run/user/UID/frametop-hands)")
    ap.add_argument("--plan", action="store_true", help="print the sections and their length, and exit")
    ap.add_argument("--auto", action="store_true",
                    help="advance by itself: each prompt for its time, no waiting for Next (the old timed flow)")
    ap.add_argument("--next-after", type=float, metavar="S",
                    help="test: press Next by itself after S seconds of waiting (real time)")
    ap.add_argument("--poses", help="the pose pictures' folder, with poses.json (default hands/rec/poses)")
    ap.add_argument("--no-headset-button", action="store_true",
                    help="don't read the headset's button (gpio-keys KEY_SELECT: Next, pause, resume)")
    ap.add_argument("--button-device", metavar="PATH",
                    help="test: read the button from this event device or FIFO of input_event structs (also in a dry run)")
    ap.add_argument("--quick", action="store_true",
                    help="a quick round (about 3 min, for another lighting): hand size, the two-hand sweeps, touch, no hands")
    ap.add_argument("--seed", type=int, help="the sweeps' shuffle (default: from the session's id; --plan: the script's order)")
    ap.add_argument("--ignore-cameras", action="store_true",
                    help="start even if the camera check (hands/camcheck.py) finds the upper cameras off")
    a = ap.parse_args()

    known = ("pencil", "phone", "cup", "keyboard", "mouse", "gamepad", "small")
    names = [o.strip() for o in a.objects.split(",") if o.strip()]
    checklist = {"objects": [o for o in names if o in known], "own_objects": [o for o in names if o not in known],
                 "controllers": "straps" if a.controllers else "none", "sleeves": "", "rings": False,
                 "watch": False, "notes": ""}
    base = a.base
    if not base and a.dry_run:
        import tempfile
        base = tempfile.mkdtemp(prefix="handrec-dry-")
    base = base or BASE_DIR
    profile = {}
    try:
        with open(os.path.join(base, "profile.json")) as f:
            profile = json.load(f)
    except (OSError, ValueError):
        pass

    t0 = time.monotonic()
    last = {}

    def on_status(st):
        key = (st["state"], st["section"], st["prompt"], st["note"], st["take"], st["waiting"], st["countdown"],
               st["hands"]["left"], st["hands"]["right"], st["error"])
        if key == last.get("key"):
            return
        last["key"] = key
        hands = "".join("%s%s" % (s[0].upper(), {True: "+", False: "-", None: "?"}[st["hands"][s]])
                        for s in ("left", "right"))
        state = "%s %d" % (st["state"], st["countdown"]) if st["state"] == "countdown" else st["state"]
        line = "[%6.1f] %-11s %d/%d %-16s %s %s" % (time.monotonic() - t0, state, st["section_index"],
                                                    st["section_count"], st["section"] or "-", hands,
                                                    st["prompt"].replace("\n", " | "))
        if st["image"]:
            line += "  [%s%s]" % (os.path.basename(st["image"]), " " + st["image_mode"] if st["image_mode"] else "")
        if st["waiting"]:
            line += "  (waiting for Next)"
        if st["note"]:
            line += "  (%s)" % st["note"]
        if st["error"]:
            line += "  ERROR: %s" % st["error"]
        print(line, flush=True)

    s = Session(base, profile, checklist, a.lighting, a.script, ring=a.ring, start_processes=not a.no_start,
                dry_run=a.dry_run, speed=a.speed, on_status=on_status, hands_dir=a.hands_dir, panel_bin=a.panel,
                auto=a.auto, next_after=a.next_after, poses_dir=a.poses, button=not a.no_headset_button,
                button_device=a.button_device, check_cameras=not a.ignore_cameras, quick=a.quick, seed=a.seed)
    est = plan_seconds(s.script, s.plan, auto=a.auto)
    worst = plan_seconds(s.script, s.plan, worst=True, auto=a.auto)
    print("%d sections%s, %s mode: about %.1f min%s (at most %.1f)%s" % (
        len(s.plan), " (a quick round)" if a.quick else "", "auto" if a.auto else "step", est / 60, "" if a.auto else " recorded", worst / 60,
        ", %gx speed" % a.speed if a.speed != 1 else ""))
    if not a.auto:
        print("  %d steps wait for Next: add your reading time (at 5 s a step, %.1f min)"
              % (plan_steps(s.plan), plan_steps(s.plan) * 5 / 60))
    for sk in s.skipped:
        print("  skipping %s: %s" % (sk["section"], sk["reason"]))
    if a.plan:
        for sec in s.plan:
            print("  %-18s %-9s %3d prompts %3d steps %5.0f s" % (sec["id"], sec["kind"], len(sec["prompts"]),
                                                                section_steps(sec), section_seconds(sec, auto=a.auto)))
            for p in sec["prompts"]:
                if p.get("cues"):
                    print("      %-26s %-5s %3.0f s  %s" % (p["id"], p["hands"], p["seconds"], " ".join(p["cues"])))
        return 0
    if not a.dry_run and not a.ring:
        cam = camera_check()
        print("cameras: %s" % cam["summary"])
        for line in cam.get("evidence", []):
            print("  " + line)
        if camera_text(cam):
            print(camera_text(cam))
            if not a.ignore_cameras:
                print("Not starting (--ignore-cameras starts anyway).")
                return 3
    if not a.dry_run:
        light = ring_lighting(a.ring)
        match = similar_lighting(base, {"chosen": a.lighting, "ring": light}) if light else None
        print("lighting: %s" % (json.dumps(light) if light else "no camera ring"))
        if light:
            print("  measured: %s (ambient IR %s)" % (classify_lighting(light) or "can't tell", ambient_ir(light)))
        if match:
            print("  about the same light as session %s (%s)" % match)

    def on_signal(*_):
        print("stopping", flush=True)
        threading.Thread(target=s.stop, daemon=True).start()

    signal.signal(signal.SIGINT, on_signal)
    signal.signal(signal.SIGTERM, on_signal)
    def keys():
        # From a terminal, or lines piped in (a test); /dev/null ends at once.
        if sys.stdin.isatty():
            print("keys (then Enter): n or just Enter next, p pause/resume, r redo, s skip section, q stop", flush=True)
        for line in sys.stdin:
            c = line.strip()[:1].lower()
            if c in ("n", ""):
                s.next_step()
            elif c == "p":
                s.resume() if s.state == "paused" else s.pause()
            elif c == "r":
                s.redo()
            elif c == "s":
                s.skip()
            elif c == "q":
                s.stop(wait=0)
    if sys.stdin is not None:
        threading.Thread(target=keys, daemon=True).start()
    s.start()
    while s._thread.is_alive():
        s.join(0.5)
    print("session: %s" % s.session_dir)
    return 0 if s.state in ("done", "stopped") else 1


if __name__ == "__main__":
    sys.exit(main())
