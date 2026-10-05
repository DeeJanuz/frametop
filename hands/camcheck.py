#!/usr/bin/env python3
"""camcheck: are the headset's four mono tracking cameras running, so that ft-camd and
ft-hands see them all?

The Frame has four mono IR tracking cameras: the side pair slam_left and slam_right
(/dev/video9 and /dev/video13) and the upper pair (/dev/video6 and /dev/video7). With the
Arcturus colour module attached, SteamVR's XRService loads an FPGA image ("VCINT") onto the
module whenever it opens the cameras (at start and after every wake). When that load fails
(seen 2026-10-02 17:02, after a sleep), XRService runs only the two side cameras, the IR
illuminator seems to stay off, and ft-hands finds no hands at all.

What it looks at, cheapest first, all read-only:
  1. The newest XRService log (~/.local/share/Steam/logs/xrservice.txt, a symlink to the
     running instance's log): the last camera start, its VCINT result, "Upper cameras FPGA
     interleaving support: N", "Created N tasks (T tracking, P passthrough)" and the
     TrackingCameraInit lines. A wake doesn't always print "Created N tasks", so the parser
     tracks each camera start ("episode") from the FPGA check to the next close.
  2. Which /dev/video* XRService has open (/proc/PID/fd). Only on the host: the dev container
     can't read another process's fd table (checked 2026-10-02), and then this is skipped.
  3. A running ft-camd's ring header (/run/user/UID/frametop-hands/cam-ring): how many mono
     cameras it publishes.

Status: "ok", "degraded: <why>" or "unknown" (SteamVR not running, the cameras closed while
the headset sleeps, no log). Exit status 0, 1, 2 for those.

  python3 hands/camcheck.py             # the status and its evidence
  python3 hands/camcheck.py --json      # for programs
  python3 hands/camcheck.py --log FILE --no-proc --no-ring   # a saved log only (tests)

System Python, standard library only; session.py and ft-camwatch import it.
"""
import argparse
import glob
import json
import os
import re
import struct
import sys
import time

LOG_DIR = os.path.expanduser("~/.local/share/Steam/logs")
LOG_LINK = os.path.join(LOG_DIR, "xrservice.txt")
SIDE_NODES = (9, 13)     # slam_left, slam_right (TrackingCameraInit index 0 and 1)
UPPER_NODES = (6, 7)     # the upper pair (index 2 and 3)
TRACKING = 4

VCINT_REASON = "upper cameras and IR light off (VCINT FPGA failed to load)"
DEGRADED_VCINT = "degraded: " + VCINT_REASON

# What the window and the recorder say when the check fails this way.
USER_TEXT = ("The headset's upper cameras and IR light are off. SteamVR couldn't start the colour camera "
             "module (it happens sometimes after the headset sleeps). Restart SteamVR, or restart the headset "
             "if that doesn't fix it.")

ANSI = re.compile(r"\x1b\[[0-9;]*m")
STAMP = re.compile(r"^\w{3} \w{3} \d{2} \d{4} (\d{2}:\d{2}:\d{2})\.\d+ (\w+): ?(.*)$")
# Lines worth reading; anything else is skipped before the regexes (the log grows by MBs a day).
KEYS = ("FPGA", "VCINT", "Created", "TrackingCameraInit", "Closing tracking camera", "Streaming",
        "systemd suspend", "systemd resume", "XRService logging to", "Exiting XRService", "ISP ")
# XRService's numbering of its tracking cameras (the TrackingCameraInit index): ft-hands names
# them this way too (track/main.cpp, cameras_from_xrservice_log).
NAMES = ("slam_left", "slam_right", "upper_left", "upper_right")
RE_PASSTHRU = re.compile(r"Passthrough connected but FPGA is (\S+) - loading VCINT")
RE_INTERLEAVE = re.compile(r"Upper cameras FPGA interleaving support: (\d)")
RE_TASKS = re.compile(r"Created (\d+) tasks \((\d+) tracking, (\d+) passthrough\)")
RE_INIT = re.compile(r"TrackingCameraInit: index: (\d+)\. video device: /dev/video(\d+)")
RE_STREAM = re.compile(r"Streaming resumed \(FPGA: (\S+), VC interleaving: (\w+)\)")
RE_STATE = re.compile(r"FPGA state check: (\S+)")
# Without the colour module XRService runs the side cameras through the ISP, as NV12 on other
# capture pipes (vfe0 and vfe1), and the upper pair on vfe3 and vfe4.
RE_ISP = re.compile(r"ISP (enabled|disabled) for tracking cameras")


class LogState:
    """Reads an XRService log line by line (feed), so the watcher can follow it as it grows.

    An episode is one opening of the cameras: from the first FPGA, task or camera-init line
    after the log starts or after "Closing tracking camera interfaces", to the next close."""

    def __init__(self, path=""):
        self.path = path
        self.instance = ""        # the "XRService logging to" line's time
        self.exited = False
        self.closed = False       # the cameras were closed and haven't opened again
        self.closed_at = ""
        self.episode = None
        self.nodes = {}           # TrackingCameraInit index -> /dev/videoN, from the whole log
        self.failures = []        # [(time, line)]: every VCINT failure in this log
        self.lines = 0

    def _new_episode(self, t):
        self.closed = False
        self.episode = {"start": t, "fpga_before": "", "vcint": "", "interleave": None, "tasks": None,
                        "inits": {}, "stream": "", "isp": None, "failure": "", "evidence": []}
        if self.closed_at:
            self.episode["evidence"].append(self.closed_at)

    def _ep(self, t):
        if self.episode is None or self.closed:
            self._new_episode(t)
        return self.episode

    def feed(self, raw):
        self.lines += 1
        if not any(k in raw for k in KEYS):
            return
        line = ANSI.sub("", raw).rstrip("\n")
        m = STAMP.match(line)
        if not m:
            return   # the FPGA loader's own output, without a time
        t, _level, text = m.groups()
        short = ("%s %s" % (t, text))[:220]
        if "XRService logging to" in text:
            lines = self.lines
            self.__init__(self.path)
            self.instance, self.lines = t, lines
            return
        if "Exiting XRService" in text:
            self.exited = True
            return
        if "Closing tracking camera interfaces" in text:
            self.closed, self.closed_at = True, short
            return
        if "systemd suspend notification" in text or "systemd resume notification" in text:
            if "resume" in text:
                self.closed_at = (self.closed_at + " / " if self.closed_at else "") + short
            return
        m = RE_PASSTHRU.search(text)
        if m:
            ep = self._ep(t)
            ep["fpga_before"] = m.group(1)
            ep["evidence"].append(short)
            return
        if "FPGA image VCINT loaded and verified successfully" in text:
            ep = self._ep(t)
            ep["vcint"] = "ok"
            ep["evidence"].append(short)
            return
        if "Failed to load VCINT FPGA image" in text:
            ep = self._ep(t)
            ep["vcint"] = "failed"
            ep["failure"] = t
            ep["evidence"].append(short)
            self.failures.append((t, short))
            return
        if "FPGA load failed" in text:
            self._ep(t)["evidence"].append(short)
            return
        m = RE_INTERLEAVE.search(text)
        if m:
            ep = self._ep(t)
            ep["interleave"] = int(m.group(1))
            ep["evidence"].append(short)
            return
        m = RE_ISP.search(text)
        if m:
            ep = self._ep(t)
            ep["isp"] = m.group(1) == "enabled"
            ep["evidence"].append(short)
            return
        m = RE_TASKS.search(text)
        if m:
            ep = self._ep(t)
            ep["tasks"] = tuple(int(v) for v in m.groups())
            ep["evidence"].append(short)
            return
        m = RE_INIT.search(text)
        if m:
            ep = self._ep(t)
            idx, node = int(m.group(1)), int(m.group(2))
            ep["inits"][idx] = node
            self.nodes[idx] = node
            ep["evidence"].append(short)
            return
        m = RE_STREAM.search(text)
        if m:
            ep = self._ep(t)
            ep["stream"] = "%s, interleaving %s" % m.groups()
            ep["evidence"].append(short)
            return
        m = RE_STATE.search(text)
        if m:
            ep = self._ep(t)
            if not ep["fpga_before"] and not ep["vcint"]:
                ep["fpga_before"] = m.group(1)
                if m.group(1) == "VCINT":
                    ep["vcint"] = "loaded"   # already there: no load needed (a SteamVR restart in the same boot)
                ep["evidence"].append(short)

    def feed_text(self, text):
        for line in text.splitlines():
            self.feed(line)
        return self

    def upper_nodes(self):
        got = tuple(self.nodes[i] for i in (2, 3) if i in self.nodes)
        return got if len(got) == 2 else UPPER_NODES

    def camera_map(self):
        """{calibration name: /dev/videoN's N} from the latest camera start's TrackingCameraInit
        lines (the whole log's when that start has none yet)."""
        inits = (self.episode or {}).get("inits") or self.nodes
        return {NAMES[i]: node for i, node in sorted(inits.items()) if 0 <= i < len(NAMES)}

    def tracking_nodes(self):
        got = tuple(self.nodes[i] for i in range(TRACKING) if i in self.nodes)
        return got if len(got) == TRACKING else SIDE_NODES + UPPER_NODES

    def verdict(self):
        """(status, reason, evidence): status "ok", "degraded" or "unknown"."""
        if self.lines == 0:
            return "unknown", "the XRService log is empty", []
        if self.exited:
            return "unknown", "XRService has exited (SteamVR isn't running)", []
        if self.episode is None:
            return "unknown", "the cameras haven't started yet in this log", []
        ep = self.episode
        ev = ep["evidence"][-14:]
        if self.closed:
            return "unknown", "the cameras are closed (the headset is asleep, or SteamVR is stopping)", \
                ev + [self.closed_at]
        tracking = len(ep["inits"]) if ep["inits"] else (ep["tasks"][1] if ep["tasks"] else None)
        if ep["vcint"] == "failed":
            return "degraded", VCINT_REASON, ev
        if tracking is not None and tracking < TRACKING:
            why = "only %d of %d tracking cameras running" % (tracking, TRACKING)
            if ep["interleave"] == 0:
                why += " (upper cameras' FPGA interleaving off)"
            return "degraded", why, ev
        if tracking == TRACKING:
            return "ok", "%d tracking cameras running" % TRACKING, ev
        return "unknown", "the cameras are starting", ev

    def snapshot(self):
        status, reason, ev = self.verdict()
        ep = self.episode or {}
        return {"status": status, "reason": reason, "evidence": ev, "log": self.path, "instance": self.instance,
                "episode": {k: (list(v) if isinstance(v, tuple) else v) for k, v in ep.items() if k != "evidence"},
                "failure": "%s@%s" % (self.path, ep["failure"]) if ep.get("vcint") == "failed" else ""}


# ------------------------------------------------------------------------------------------
# Processes

def proc_argv(pid):
    try:
        with open("/proc/%s/cmdline" % pid, "rb") as f:
            return [a.decode(errors="replace") for a in f.read().split(b"\0") if a]
    except OSError:
        return []


def xrservice_pid():
    """XRService's pid (its main thread renames itself XRServiceLoopTh), or None."""
    for pid in os.listdir("/proc"):
        if not pid.isdigit():
            continue
        try:
            with open("/proc/%s/comm" % pid) as f:
                if not f.read().startswith("XRService"):
                    continue
        except OSError:
            continue
        argv = proc_argv(pid)
        if argv and os.path.basename(argv[0]) == "XRService":
            return int(pid)
    return None


def xrservice_fds(pid):
    """{"videos": [N, ...], "log": path or ""} from /proc/PID/fd, or None if it can't be read
    (the dev container can't)."""
    try:
        fds = os.listdir("/proc/%d/fd" % pid)
    except OSError:
        return None
    videos, log = set(), ""
    for fd in fds:
        try:
            target = os.readlink("/proc/%d/fd/%s" % (pid, fd))
        except OSError:
            continue
        m = re.match(r"/dev/video(\d+)$", target)
        if m:
            videos.add(int(m.group(1)))
        elif re.search(r"/XRService-[^/]*\.log$", target):
            log = target
    if not videos and not log:
        return None   # nothing readable: as good as no access
    return {"videos": sorted(videos), "log": log}


def newest_log():
    """The running XRService's log: the xrservice.txt symlink, else the newest by time."""
    if os.path.exists(LOG_LINK):
        return os.path.realpath(LOG_LINK)
    found = glob.glob(os.path.join(LOG_DIR, "XRService-*", "XRService-*.log"))
    found += glob.glob(os.path.join(LOG_DIR, "XRService-*.log"))
    found = [p for p in found if os.path.isfile(p)]
    return max(found, key=os.path.getmtime) if found else ""


def read_log(path):
    st = LogState(path)
    with open(path, "r", errors="replace") as f:
        for line in f:
            st.feed(line)
    return st


# ------------------------------------------------------------------------------------------
# ft-camd's ring (camd/fhring.h; the header only, as session.py's Ring reads it)

RING_HDR = struct.Struct("<8sIIIIQqQ16x")
RING_CAM = struct.Struct("<32s32siIIIIIQQQQQIf24x")
FH_CAM_DARK, FH_CAM_COLOR = 1, 2


def default_ring():
    return "/run/user/%d/frametop-hands/cam-ring" % os.getuid()


def read_ring(path):
    """{"alive", "writer_pid", "mono": [{"name", "sensor", "node"}]} or None (no ring)."""
    try:
        with open(path, "rb") as f:
            data = f.read(RING_HDR.size + 8 * RING_CAM.size)
    except OSError:
        return None
    if len(data) < RING_HDR.size:
        return None
    magic, version, _, ncams, _, _, writer, _ = RING_HDR.unpack_from(data, 0)
    if magic != b"FHRING01" or version != 1:
        return None
    hb = struct.unpack_from("<Q", data, 40)[0]
    alive = hb != 0 and (time.clock_gettime_ns(time.CLOCK_MONOTONIC) - hb) / 1e9 < 2.0
    mono = []
    for i in range(min(ncams, 8)):
        off = RING_HDR.size + i * RING_CAM.size
        if off + RING_CAM.size > len(data):
            break
        f = RING_CAM.unpack_from(data, off)
        sensor = f[0].split(b"\0", 1)[0].decode(errors="replace")
        name = f[1].split(b"\0", 1)[0].decode(errors="replace")
        if f[13] & (FH_CAM_DARK | FH_CAM_COLOR) or name.endswith("_dk") or name.startswith("color"):
            continue
        mono.append({"name": name, "sensor": sensor, "node": f[2]})
    return {"alive": alive, "writer_pid": writer, "mono": mono}


# ------------------------------------------------------------------------------------------
# The check

def check(log=None, proc=True, ring=True, ring_path=None):
    """The cameras' state: {"status": "ok"|"degraded"|"unknown", "summary": "ok" or
    "degraded: ..." or "unknown: ...", "reason", "evidence": [lines], "log", "xrservice", "ring"}."""
    evidence = []
    pid = xrservice_pid() if proc else None
    fds = xrservice_fds(pid) if pid else None
    path = log or (fds or {}).get("log") or newest_log()
    state = None
    if path:
        try:
            state = read_log(path)
        except OSError as e:
            evidence.append("log %s: %s" % (path, e))
    if state:
        status, reason, ev = state.verdict()
        evidence += ["log %s:" % path] + ["  " + e for e in ev]
    else:
        status, reason = "unknown", "no XRService log in %s" % LOG_DIR
    out = {"log": path, "xrservice": None, "ring": None,
           "episode": state.snapshot()["episode"] if state else {},
           "failure": state.snapshot()["failure"] if state else "",
           "map": {name: {"node": n, "pipe": pipe_name(n)} for name, n in state.camera_map().items()} if state else {},
           "ring_missing": []}

    if proc:
        if pid is None:
            # The log can't tell a killed XRService from a running one; no process settles it.
            evidence.append("XRService isn't running")
            status, reason = "unknown", "SteamVR isn't running (no XRService)"
        elif fds is None:
            evidence.append("XRService pid %d: its open files can't be read here (in the dev container?)" % pid)
            out["xrservice"] = {"pid": pid, "videos": None}
        else:
            videos = fds["videos"]
            want = state.tracking_nodes() if state else SIDE_NODES + UPPER_NODES
            upper = state.upper_nodes() if state else UPPER_NODES
            have = [n for n in want if n in videos]
            evidence.append("XRService pid %d has open: %s (tracking cameras: %s; upper: %s)" % (
                pid, " ".join("video%d" % n for n in videos) or "no cameras",
                " ".join("video%d" % n for n in want), " ".join("video%d" % n for n in upper)))
            out["xrservice"] = {"pid": pid, "videos": videos, "tracking_open": len(have)}
            closed = state is not None and state.closed
            if not closed and len(have) == TRACKING and status == "unknown":
                status, reason = "ok", "XRService has all %d tracking cameras open" % TRACKING
            elif not closed and videos and not all(n in videos for n in upper) and status != "degraded":
                status, reason = "degraded", ("XRService has %d of %d tracking cameras open (the upper pair "
                                              "is missing)" % (len(have), TRACKING))

    if ring:
        r = read_ring(ring_path or default_ring())
        out["ring"] = r
        if r is None:
            evidence.append("ft-camd: no camera ring (not running)")
        else:
            names = " ".join(c["name"] for c in r["mono"]) or "none"
            evidence.append("ft-camd (pid %d, %s): %d mono cameras: %s" % (
                r["writer_pid"], "running" if r["alive"] else "stale ring", len(r["mono"]), names))
            if r["alive"] and len(r["mono"]) < TRACKING and status == "ok":
                have = {c["node"] for c in r["mono"]}
                want = state.tracking_nodes() if state else SIDE_NODES + UPPER_NODES
                out["ring_missing"] = [n for n in want if n not in have]
                status, reason = "degraded", ("ft-camd publishes only %d of %d mono cameras (missing: %s)" % (
                    len(r["mono"]), TRACKING, " ".join("video%d" % n for n in out["ring_missing"]) or "?"))
    out.update(status=status, reason=reason, evidence=evidence,
               summary="ok" if status == "ok" else "%s: %s" % (status, reason))
    return out


def pipe_name(node):
    """The capture pipe behind /dev/videoN (msm_vfe3_video0 and so on), or ""."""
    try:
        with open("/sys/class/video4linux/video%d/name" % node) as f:
            return f.read().strip()
    except OSError:
        return ""


def is_ring_short(result):
    """XRService runs all the tracking cameras, but ft-camd doesn't publish them all."""
    return bool(result) and result.get("status") == "degraded" and bool(result.get("ring_missing"))


def is_vcint_failure(result):
    return bool(result) and result.get("status") == "degraded" and result.get("reason") == VCINT_REASON


def main(argv=None):
    ap = argparse.ArgumentParser(description="Are the headset's four mono tracking cameras running?")
    ap.add_argument("--json", action="store_true", help="print the result as JSON")
    ap.add_argument("--log", help="read this XRService log (default: the running instance's)")
    ap.add_argument("--no-proc", action="store_true", help="don't look at XRService's process")
    ap.add_argument("--no-ring", action="store_true", help="don't look at ft-camd's ring")
    ap.add_argument("--ring", help="ft-camd's ring (default /run/user/UID/frametop-hands/cam-ring)")
    a = ap.parse_args(argv)
    r = check(log=a.log, proc=not a.no_proc, ring=not a.no_ring, ring_path=a.ring)
    if a.json:
        print(json.dumps(r, indent=1))
    else:
        print(r["summary"])
        for line in r["evidence"]:
            print("  " + line)
    return {"ok": 0, "degraded": 1}.get(r["status"], 2)


if __name__ == "__main__":
    sys.exit(main())
