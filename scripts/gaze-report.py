#!/usr/bin/env python3
"""Why the gaze pointer or its calibration isn't working: what Frametop's gaze needs, checked
in one report, with what looks wrong first. scripts/report.sh includes it. On its own, in a
terminal on the Frame (or `ssh frame python3 - < scripts/gaze-report.py`):
  scripts/gaze-report.py [--no-wake]
It prints:
  - findings: what looks wrong, from everything below
  - the install: the gaze service's unit and the checkout it runs from, its builds (missing,
    or older than their sources), and our eye tracker's frame grabber (installed, the same
    as the build, running)
  - SteamVR's eye tracker: its process, whether it writes eye-server.mmap and in a layout
    ft-gaze knows, and its log, with repeated lines folded
  - our eye tracker: its shared files, its calibration, and ft-eyes' own status
  - the gaze service's status, the pointer helper's gaze mode, and a live check: unless
    --no-wake, an idle gaze service is woken for about 20 s (as the Gaze page of Frametop
    Input Settings does), to see whether ft-gaze starts and samples come in
  - the last checks and calibrations, with why dots weren't taken
  - the gaze settings and the buttons and keys mapped to gaze actions
  - the logs: the gaze service's, and the gaze lines of the pointer helper's, the relay's,
    the frame grabber's, and SteamVR's
Bluetooth addresses and the headset's serial number are masked. Nothing here writes to
SteamVR, its eye tracker, or ours; the wake is a lease that runs out by itself.
"""
import contextlib
import importlib.util
import io
import json
import os
import re
import socket
import statistics
import struct
import subprocess
import sys
import time
from collections import Counter
from pathlib import Path

HOME = Path.home()
UID = os.getuid()
# A terminal in the desktop has its session's runtime dir, bus, and config folder; systemctl
# and SteamVR's tools need the real ones (see scripts/update-check.py).
os.environ.update(XDG_RUNTIME_DIR=f"/run/user/{UID}", DBUS_SESSION_BUS_ADDRESS=f"unix:path=/run/user/{UID}/bus",
                  XDG_CONFIG_HOME=str(HOME / ".config"))
HERE = Path(__file__).resolve().parents[1] if Path(__file__).is_file() else None  # None when read from stdin
UNIT = "frametop-gaze"
STATE = HOME / ".local/state/frametop/gaze"
CONF = HOME / ".config/frametop.conf"
INPUT = HOME / ".config/frametop-input.json"
LOGS = HOME / ".local/share/Steam/logs"
EYE_MMAP = Path("/dev/shm/eye-server.mmap")
CAMS = Path("/dev/shm/frametop-eyes-cams")    # ft-eyegrab's copies of the eye cameras
WANT = Path("/dev/shm/frametop-eyes-want")    # ft-eyes touches it every second while it wants them
OWN_GAZE = Path("/dev/shm/frametop-eyes-gaze")  # ft-eyes' gaze (its layout: gaze/tracker/ft-eyes)
EYEGRAB_BIN = Path("/etc/frametop/ft-eyegrab")
EYEGRAB_UNIT = Path("/etc/systemd/system/frametop-eyegrab.service")
WAKE = 20        # seconds the live check keeps an idle gaze service awake
RATE_SECS = 3.0  # seconds the live check counts samples over
LOST_SHARE = 0.4  # an eye lost in more of the samples than this is a finding

out = []       # the report's details, printed after the findings
findings = []


def say(*lines):
    out.extend(str(line) for line in lines)


def section(title):
    out.append(f"\n===== Gaze: {title}")


def find(msg):
    findings.append(msg)


def run(*cmd, timeout=10):
    """(exit code, stdout stripped); -1 when it can't run."""
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, stdin=subprocess.DEVNULL)
        return p.returncode, p.stdout.strip()
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, str(e)


def ask(name, msg, timeout=1.0):
    """A command on an abstract datagram socket. The reply; None when nothing listens; "" when
    nothing answered in time."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM | socket.SOCK_CLOEXEC)
    s.bind(f"\0ft_gazereport.{os.getpid()}")
    s.settimeout(timeout)
    try:
        s.sendto(msg.encode(), "\0" + name)
        return s.recv(65536).decode("utf-8", "replace")
    except (ConnectionRefusedError, FileNotFoundError):
        return None
    except socket.timeout:
        return ""
    finally:
        s.close()


def ask_json(name, msg, timeout=1.0):
    r = ask(name, msg, timeout)
    try:
        return json.loads(r) if r else r
    except ValueError:
        return r


def unit_props(unit, props, user=True):
    """systemctl show's properties as a dict."""
    _, text = run("systemctl", *(["--user"] if user else []), "show", unit, "-p", ",".join(props))
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line)


def mtime(path):
    try:
        return Path(path).stat().st_mtime
    except OSError:
        return None


def ago(t):
    """How long ago an epoch time was, in words."""
    if t is None:
        return "never"
    s = time.time() - t
    for unit, size in (("d", 86400), ("h", 3600), ("min", 60)):
        if s >= size:
            return f"{s / size:.0f} {unit} ago"
    return f"{s:.0f} s ago"


def stamp(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)) if t else "none"


def git(repo, *args):
    rc, text = run("git", "-C", str(repo), *args)
    return text if rc == 0 else None


def fold(entries):
    """(key, text, time) entries, consecutive ones with the same key as one, with a count."""
    folded = []
    for key, text, t in entries:
        if folded and folded[-1][0] == key:
            folded[-1][2] += 1
            folded[-1][3] = t
        else:
            folded.append([key, text, 1, t])
    return [text + (f"  (x{n}, last {last})" if n > 1 else "") for _, text, n, last in folded]


JOURNAL = re.compile(r"^(\w{3} \d\d \d\d:\d\d:\d\d) \S+ ([^:\[]+)(?:\[\d+\])?: (.*)$")


def journal(*args, grep=None, last=60, drop=None):
    """journalctl lines (this boot), folded; the host name and the service's PID left out."""
    _, text = run("journalctl", *args, "-b", "--no-pager", "-o", "short", timeout=20)
    entries = []
    for line in text.splitlines():
        m = JOURNAL.match(line)
        if not m:
            continue
        t, ident, msg = m.groups()
        if drop and re.search(drop, ident + ": " + msg):
            continue
        if grep and not re.search(grep, msg, re.I):
            continue
        shown = msg if ident in ("python3", "distrobox") or msg.startswith(ident + ":") else f"{ident}: {msg}"
        entries.append((shown, f"{t} {shown}", t[7:]))
    return (fold(entries)[-last:] if last else fold(entries)) or ["(nothing)"]


STEAM_LOG = re.compile(r"^\w{3} (\w{3} \d\d) \d{4} (\d\d:\d\d:\d\d)\.\d+ \[(\w+)\] - (.*)$")


def steam_log(path):
    """A SteamVR log as (message, shown, time), without its banner lines."""
    try:
        text = Path(path).read_text(errors="replace")
    except OSError:
        return None
    entries = []
    for line in text.splitlines():
        m = STEAM_LOG.match(line)
        if not m or m.group(4).lstrip("/").startswith("==="):
            continue
        day, t, level, msg = m.groups()
        shown = f"{day} {t} " + (f"[{level}] " if level != "Info" else "") + msg
        entries.append((msg, shown, t))
    return entries


# --- The install ---

def check_install():
    """The gaze service's unit and checkout, its builds, and our tracker's frame grabber.
    Returns (the service's checkout, its unit's properties)."""
    section("install")
    rc, text = run("systemctl", "--user", "cat", UNIT)
    unit_file = next((line[2:] for line in text.splitlines() if line.startswith("# /")), None)
    exec_start = next((line.split("=", 1)[1] for line in text.splitlines() if line.startswith("ExecStart=")), None)
    props = unit_props(UNIT, ("LoadState", "UnitFileState", "ActiveState", "SubState", "Result", "NRestarts",
                              "ExecMainStatus", "ActiveEnterTimestamp"))
    steamvr = run("systemctl", "--user", "is-active", "steamvr")[1]
    try:
        osr = dict(line.split("=", 1) for line in Path("/etc/os-release").read_text().splitlines() if "=" in line)
        say(f"SteamOS {osr.get('VERSION_ID', '?')} build {osr.get('BUILD_ID', '?')}".replace('"', ""))
    except OSError:
        pass
    say(f"SteamVR (steamvr.service): {steamvr}")
    if rc != 0 or not unit_file:
        say(f"{UNIT}.service: not installed")
        find("The gaze service isn't installed: run gaze/run.sh install (install.sh offers it as a step)")
        return HERE, props
    say(f"unit: {unit_file}", f"runs: {exec_start}",
        "state: " + ", ".join(f"{k}={v}" for k, v in props.items() if v))
    m = re.search(r"(\S+)/gaze/ft-gazed", exec_start or "")
    repo = Path(m.group(1)).resolve() if m else HERE
    if props.get("UnitFileState") not in ("enabled", "linked"):
        find(f"The gaze service isn't enabled ({props.get('UnitFileState')}), so it doesn't start with SteamVR: "
             "run gaze/run.sh install")
    if props.get("ActiveState") != "active":
        if steamvr != "active":
            find("SteamVR isn't running, and the gaze service only runs with it")
        else:
            find(f"The gaze service isn't running ({props.get('ActiveState')}, result {props.get('Result')}, "
                 f"exit status {props.get('ExecMainStatus')}): see its log below")
    if props.get("NRestarts", "0") not in ("", "0"):
        find(f"The gaze service restarted {props['NRestarts']} times since SteamVR started it (ft-gazed crashed?): "
             "see its log below")

    if not repo or not repo.is_dir():
        find(f"The gaze service's checkout {repo} is gone: run gaze/run.sh install from the checkout you use")
        return repo, props
    commit = git(repo, "log", "-1", "--format=%h %cd", "--date=short")
    branch = git(repo, "rev-parse", "--abbrev-ref", "HEAD")
    dirty = git(repo, "status", "--porcelain", "--untracked-files=no")
    say(f"checkout: {repo} ({branch or '?'} {commit or 'not a git checkout'}"
        + (", with local changes" if dirty else "") + ")")
    if HERE and HERE != repo:
        here = git(HERE, "log", "-1", "--format=%h", "--date=short")
        say(f"this report's checkout: {HERE} ({here or '?'})")
        find(f"The gaze service runs from {repo}, not from {HERE}, where this report is: "
             "if you updated or changed this one, run gaze/run.sh install from it")

    # Builds, each against its sources: a pull that changed one without a rebuild runs old code.
    say("builds:")
    builds = [("gaze/build/ft-gaze", ["gaze/ft-gaze.cpp", "pointer/common"], "gaze/build.sh"),
              ("gaze/build/ft-gazepanel", ["gaze/panel/ft-gazepanel.cpp"], "gaze/build.sh"),
              ("gaze/tracker/build/ft-eyegrab", ["gaze/tracker/ft-eyegrab.c"], "gaze/tracker/build.sh")]
    for built, sources, how in builds:
        t = mtime(repo / built)
        newer = [s for s in sources for f in ([repo / s] if (repo / s).is_file() else (repo / s).glob("*"))
                 if (mtime(f) or 0) > (t or 0)]
        say(f"  {built}: " + ("missing" if t is None else stamp(t))
            + (f"; older than {', '.join(dict.fromkeys(newer))}" if t and newer else ""))
        if built.endswith("ft-eyegrab"):
            continue  # only our tracker needs it; check_own() judges that
        if t is None:
            find(f"{built} isn't built: run {how}")
        elif newer:
            find(f"{built} is older than its source: run {how}, then gaze/run.sh restart")
    venv = repo / "gaze/tracker/build/venv"
    done = venv / "requirements.done"
    req = repo / "gaze/tracker/requirements.txt"
    if not (venv / "bin/python").exists():
        say("  gaze/tracker/build/venv (our tracker's Python): missing")
    else:
        same = done.is_file() and req.is_file() and done.read_bytes() == req.read_bytes()
        say(f"  gaze/tracker/build/venv (our tracker's Python): {stamp(mtime(done) or mtime(venv))}"
            + ("" if same else "; made from an older requirements.txt"))

    # Our tracker's frame grabber: installed with sudo, so a rebuild alone doesn't reach it.
    say("frame grabber (our eye tracker, gaze/tracker/install.sh):")
    say(f"  {EYEGRAB_BIN}: " + (stamp(mtime(EYEGRAB_BIN)) if EYEGRAB_BIN.exists() else "missing"))
    say(f"  {EYEGRAB_UNIT}: " + ("present" if EYEGRAB_UNIT.exists() else "missing"))
    build = repo / "gaze/tracker/build/ft-eyegrab"
    if EYEGRAB_BIN.exists() and build.exists():
        try:
            same = EYEGRAB_BIN.read_bytes() == build.read_bytes()
            say("  installed = this checkout's build" if same else
                "  installed differs from this checkout's build (built after the last gaze/tracker/install.sh?)")
        except OSError as e:
            say(f"  can't compare with the build: {e}")
    try:
        doc = next((line for line in EYEGRAB_UNIT.read_text().splitlines() if line.startswith("Documentation=")), "")
        if doc:
            say(f"  installed from: {doc.split('file://', 1)[-1].removesuffix('/gaze/README.md')}")
    except OSError:
        pass
    eg = unit_props("frametop-eyegrab", ("UnitFileState", "ActiveState", "SubState", "Result", "NRestarts"),
                    user=False)
    say("  frametop-eyegrab.service: " + ", ".join(f"{k}={v}" for k, v in eg.items() if v))
    return repo, props


def gaze_settings(repo):
    """frametop.conf's gaze lines (set or default), and which tracker that makes."""
    conf = {}
    try:
        for line in CONF.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if "=" in line:
                k, v = line.split("=", 1)
                conf[k.strip()] = v.strip()
    except OSError:
        pass
    setting = conf.get("GAZE_TRACKER", "auto").lower()
    venv = (repo / "gaze/tracker/build/venv/bin/python").exists() if repo else False
    installed = EYEGRAB_BIN.exists() and EYEGRAB_UNIT.exists() and venv
    tracker = setting if setting in ("own", "steam") else "own" if installed else "steam"
    return conf, setting, installed, tracker


# --- SteamVR's eye tracker ---

def check_steam_tracker(worn, repo):
    section("SteamVR's eye tracker")
    rc, line = run("pgrep", "-a", "-x", "eyetracking")
    vrserver = run("pgrep", "-x", "vrserver")[0] == 0
    if rc == 0:
        pid = line.split()[0]
        up = run("ps", "-o", "etimes=", "-p", pid)[1]
        say(f"process: {line[:160]}", f"running for {int(up) // 60 if up.isdigit() else '?'} min")
    else:
        say("process: not running")
        if vrserver:
            find("SteamVR runs, but its eye tracker (eyetracking) doesn't: see eyetracking.txt below")
    if EYE_MMAP.exists():
        st = EYE_MMAP.stat()
        say(f"{EYE_MMAP}: {st.st_size} bytes, made {stamp(st.st_mtime)}")
    else:
        say(f"{EYE_MMAP}: missing")

    # Is it written now, and in a layout ft-gaze knows: update-check.py's own check.
    path = next((p / "scripts/update-check.py" for p in (HERE, repo, HOME / "frametop")
                 if p and (p / "scripts/update-check.py").is_file()), None)
    if path:
        spec = importlib.util.spec_from_file_location("update_check", path)
        uc = importlib.util.module_from_spec(spec)
        buf = io.StringIO()
        try:
            spec.loader.exec_module(uc)
            with contextlib.redirect_stdout(buf):
                uc.check_eye_tracker()
        except Exception as e:  # an older or changed update-check.py: the rest still runs
            buf.write(f"update-check.py's eye tracker check failed: {e}\n")
        result = buf.getvalue().strip()
        say(f"layout (scripts/update-check.py): {result}")
        if result.startswith("FAIL"):
            find("SteamVR's eye data: " + result.split(":", 1)[-1].strip())
        elif "idle" in result and worn:
            find("The headset is worn, but SteamVR's eye tracker isn't writing eye-server.mmap: "
                 "see eyetracking.txt below (SteamVR restarts it with SteamVR)")

    say("", "vrserver.txt, eye tracking (last 10):")
    entries = steam_log(LOGS / "vrserver.txt") or []
    say(*fold([e for e in entries if re.search(r"cv: Eye|eyetracking (disconnected|exited|crash)", e[0])])[-10:]
        or ["(nothing)"])

    entries = steam_log(LOGS / "eyetracking.txt")
    if entries is None:
        say("", "eyetracking.txt: missing")
        return
    starts = [i for i, e in enumerate(entries) if " startup with PID" in e[0]]
    current = entries[starts[-1]:] if starts else entries
    usercal = sum("Accept usercal" in e[0] for e in current)
    say("", f"eyetracking.txt: {len(starts)} start(s) in this log; since the last one, {usercal} clicks taken "
        "as calibration (Accept usercal); folded, last 40:")
    say(*fold(entries)[-40:])
    failed = [e for e in current if re.search(r"init failed|Failed to (initialize|load)", e[0])]
    if failed:
        find(f"SteamVR's eye tracker failed to start this time ({failed[0][0]}): SteamVR runs it, so restarting "
             "SteamVR starts it again")
    previous = steam_log(LOGS / "eyetracking.previous.txt") or []
    bad = [e for e in previous if re.search(r"fail|error|startup|shutting", e[0], re.I)
           and "grab cdsp input buffer" not in e[0]]
    if bad:
        say("", "eyetracking.previous.txt, starts and failures (folded, last 10):", *fold(bad)[-10:])


# --- Our eye tracker ---

def check_own(repo, tracker, setting, installed):
    section("our eye tracker")
    say(f"GAZE_TRACKER={setting} -> {tracker}; ours installed: {'yes' if installed else 'no'} "
        "(the frame grabber, its unit, and the venv)")
    if tracker != "own" and not installed:
        say("not in use and not installed: skipped")
        return
    if tracker == "own" and not installed:
        find("Eye tracker is Own tracker (GAZE_TRACKER=own), but ours isn't installed: run gaze/tracker/install.sh "
             "(asks for sudo), or pick SteamVR on the Gaze page")
    eg = run("systemctl", "is-active", "frametop-eyegrab")[1]
    if tracker == "own" and installed and eg != "active":
        find(f"Our tracker's frame grabber (frametop-eyegrab) is {eg}: run gaze/tracker/install.sh again")
    build = repo / "gaze/tracker/build/ft-eyegrab" if repo else None
    if tracker == "own" and installed and build and build.exists():
        try:
            if EYEGRAB_BIN.read_bytes() != build.read_bytes():
                find("The installed frame grabber isn't the one built in the service's checkout: "
                     "run gaze/tracker/install.sh to install the new one")
        except OSError:
            pass
    for path, what in ((CAMS, "the frames"), (WANT, "ft-eyes' request for frames"), (OWN_GAZE, "ft-eyes' gaze")):
        try:
            st = path.stat()
            owner = "you" if st.st_uid == UID else f"uid {st.st_uid}"
            say(f"{path} ({what}): {st.st_size} bytes, {oct(st.st_mode & 0o777)}, owner {owner}, "
                f"modified {ago(st.st_mtime)}")
            if st.st_uid != UID:
                find(f"{path} belongs to uid {st.st_uid}, not you, so ft-eyes can't use it")
        except OSError:
            say(f"{path} ({what}): missing")
    try:
        data = OWN_GAZE.read_bytes()[:32]
        _, _, t, _, _, flags, n = struct.unpack_from("<IIdffII", data)
        age = time.clock_gettime(time.CLOCK_MONOTONIC_RAW) - t
        eyes = [name for bit, name in ((2, "left"), (1, "right")) if flags & bit]
        say(f"last gaze from ft-eyes: {age:.1f} s ago, {n} published since it started, eyes in it: "
            f"{', '.join(eyes) or 'none'}")
    except (OSError, struct.error):
        pass
    cal = None
    try:
        cal = json.loads((STATE / "eyes/calibration.json").read_text()).get("info")
    except (OSError, ValueError):
        pass
    say("calibration (eyes/calibration.json): " + (json.dumps(cal) if cal else "none"))
    try:
        clicks = (STATE / "eyes/clicks.jsonl").read_text().splitlines()
        last = json.loads(clicks[-1]).get("time") if clicks else None
        say(f"clicks taught (eyes/clicks.jsonl): {len(clicks)}, the last {ago(last)}")
    except (OSError, ValueError):
        pass
    st = ask_json("ft_eyes", "status")
    if st is None:
        say("ft-eyes: not running (the gaze service runs it while the gaze is used)")
    elif isinstance(st, dict):
        say(f"ft-eyes status: {json.dumps(st, separators=(',', ':'))}")
    else:
        say(f"ft-eyes status: {st or 'no answer'}")


# --- The gaze service, live ---

def show_status(st):
    for k, v in st.items():
        say(f"  {k}: {json.dumps(v, separators=(',', ':'))}")


def check_service(props, wake):
    """Its status, the pointer helper's gaze mode, and a live check. Returns whether the
    headset is worn (None: unknown) and the status."""
    section("the service now")
    helper = ask("ft_pointer_helper", "gaze ? headset")
    worn = None
    if helper is None:
        say("pointer helper: not running")
        find("The pointer helper (frametop-pointer) isn't running: gaze mode lives there, and the gaze service "
             "idles without it")
    else:
        say(f"pointer helper, gaze mode and headset: {helper or 'no answer'}")
        words = helper.split()
        worn = "worn" in words if any(w in words for w in ("worn", "away")) else None
    st = ask_json("ft_gazed", "status")
    if st is None:
        say("ft-gazed: not running")
        if props.get("ActiveState") == "active":
            find("The gaze service's unit is active, but ft-gazed doesn't answer on @ft_gazed: see its log below")
        return worn, None
    if not isinstance(st, dict):
        say(f"ft-gazed: {st or 'no answer in 1 s'}")
        find("The gaze service doesn't answer status (busy, or stuck): see its log below")
        return worn, None
    say("ft-gazed status:")
    show_status(st)
    checks = st.get("checks") or {}
    if checks.get("problem"):
        find(f"The Gaze page says: {checks['problem']}")
    elif checks.get("calibrated") is False:
        find("The eye tracker in use has no calibration: Calibrate on the Gaze page of Frametop Input Settings")
    if checks.get("panel") is False:
        find("The calibration panel (ft-gazepanel) isn't running, so no check or calibration can show: "
             "see the log below (gaze/build.sh builds it)")
    if st.get("tracker") == "steam" and st.get("kind") == "source":
        find("SteamVR's tracker is used without a per-eye calibration (an old one, or none): Calibrate on the "
             "Gaze page")
    if st.get("tracker_setting") == "auto" and st.get("tracker") == "steam" and not st.get("own_installed"):
        say("note: GAZE_TRACKER=auto uses SteamVR's tracker, since ours isn't installed")
    n = st.get("samples") or 0
    if n >= 900:
        for side in ("left", "right"):
            share = (st.get(f"lost_{side}") or 0) / n
            if share > LOST_SHARE:
                find(f"The tracker lost your {side} eye in {share:.0%} of {n} samples since the gaze service "
                     "started: Check headset fit on the Gaze page (or glasses, or the headset's position)")

    if not wake:
        return worn, st
    section("live check")
    started = time.time()
    since = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(started - 1))
    if not st.get("awake"):
        r = ask("ft_gazed", f"wake {WAKE}")
        say(f"the gaze service idled ({st.get('idle')}): woken for {WAKE} s: {r or 'no answer'}")
    else:
        say("the gaze service was awake already")
    # Until samples come in, or with the headset off (none can) until ft-gaze and our tracker are up.
    up = None
    for _ in range(WAKE):
        s = ask_json("ft_gazed", "status")
        if not isinstance(s, dict):
            break
        if s.get("ft_gaze") and s.get("sample_age_s") is not None and s["sample_age_s"] < 1:
            up = time.time() - started
            break
        if worn is False and s.get("ft_gaze") and (s.get("kind") != "own" or s.get("own_running")):
            break
        time.sleep(1)
    s1 = ask_json("ft_gazed", "status")
    if worn is not False:
        time.sleep(RATE_SECS)
    s2 = ask_json("ft_gazed", "status")
    if not (isinstance(s1, dict) and isinstance(s2, dict)):
        say("the gaze service stopped answering during the check")
        find("The gaze service stopped answering during the live check: see its log below")
        return worn, st
    say(f"ft-gaze running: {s2.get('ft_gaze')}, our tracker answering: {s2.get('own_running')}, "
        f"eyes seen (SteamVR): {(s2.get('checks') or {}).get('eyes')}, eyes lost: {s2.get('eyes_lost')}, "
        f"headset on (SteamVR's log): {s2.get('headset_on')}, worn (the pointer helper): {worn}")
    if worn is False:
        say("The headset seemed off, so no samples were expected: run this again wearing it to test the tracker.")
    else:
        rate = ((s2.get("samples") or 0) - (s1.get("samples") or 0)) / RATE_SECS
        sent = ((s2.get("sent") or 0) - (s1.get("sent") or 0)) / RATE_SECS
        say(f"samples coming in: {'after %.0f s' % up if up is not None else 'none within %d s' % WAKE}",
            f"over {RATE_SECS:.0f} s: {rate:.0f} usable samples a second (about 90 with the headset worn and the "
            f"eyes seen), {sent:.0f} sent to the pointer")
        if rate < 10:
            why = ("ft-gaze isn't running" if not s2.get("ft_gaze") else
                   "SteamVR's tracker sees no eyes" if not (s2.get("checks") or {}).get("eyes") else
                   "our tracker sends no gaze (not calibrated, or no frames)" if s2.get("kind") == "own" else
                   "the samples come without a gaze")
            find(f"With the headset worn, the gaze service got {rate:.0f} usable samples a second (about 90 "
                 f"expected): {why}. The log since the check is below")
    say("", "the gaze service's log since the check:",
        *journal("--user", "-u", UNIT, "--since", since, drop=r"^podman", last=40))
    say("", "the frame grabber's log since the check:",
        *journal("-u", "frametop-eyegrab", "--since", since, last=10))
    return worn, s2


# --- Checks and calibrations ---

def check_history():
    section("checks and calibrations")
    try:
        meta = json.loads((STATE / "calibration.json").read_text()).get("_meta") or {}
        say(f"SteamVR's tracker, calibration.json: {meta.get('how', '?')} {meta.get('model', '?')}, "
            f"made {stamp(meta.get('calibrated_at'))}")
    except (OSError, ValueError):
        say("SteamVR's tracker, calibration.json: none")
    try:
        lines = (STATE / "checks.jsonl").read_text().splitlines()
    except OSError:
        say("checks.jsonl: none yet (no check or calibration has run)")
        return
    recs = []
    for line in lines[-2000:]:
        try:
            recs.append(json.loads(line))
        except ValueError:
            pass
    # A run: records of one kind with no gap over 2 minutes, and dots that don't start over.
    runs = []
    for r in recs:
        prev = runs[-1][-1] if runs else {}
        if (prev.get("check") == r.get("check") and r.get("time", 0) - prev.get("time", 0) < 120
                and r.get("dot", 0) >= prev.get("dot", 0)):
            runs[-1].append(r)
        else:
            runs.append([r])
    say(f"checks.jsonl: {len(lines)} records; the last {min(len(runs), 8)} runs:")
    for one in runs[-8:]:
        taken = [r for r in one if r.get("accepted")]
        reasons = Counter(r.get("reason") or r.get("reply") or "?" for r in one if not r.get("accepted"))
        per_eye = []
        for k, side in ((0, "left"), (1, "right")):
            vals = [r["miss"][k] for r in taken if isinstance(r.get("miss"), list) and len(r["miss"]) == 2
                    and r["miss"][k] is not None]
            if vals:
                per_eye.append(f"{side} {statistics.median(vals):.1f}")
        line = (f"  {stamp(one[0].get('time'))} {one[0].get('check')} "
                f"({'ours' if one[0].get('own') else 'SteamVR'}): {len(taken)} taken, "
                f"{len(one) - len(taken)} not")
        if per_eye:
            line += f"; eyes off before it (median deg): {', '.join(per_eye)}"
        say(line)
        for why, n in reasons.most_common(3):
            say(f"      not taken x{n}: {why}")
    last_full = next((one for one in reversed(runs) if one[0].get("check") == "full"), None)
    if last_full and sum(r.get("accepted", False) for r in last_full) < 2 * len({r.get("dot") for r in last_full}) / 3:
        reasons = Counter(r.get("reason") or r.get("reply") or "?" for r in last_full if not r.get("accepted"))
        top = reasons.most_common(1)[0][0] if reasons else "?"
        find(f"The last full calibration ({stamp(last_full[0].get('time'))}) took under two thirds of its dots; "
             f"most often: {top}")


# --- Settings and mappings ---

KEYS = {1: "Esc", 15: "Tab", 28: "Enter", 29: "Ctrl", 42: "Shift", 54: "RightShift", 56: "Alt", 57: "Space",
        97: "RightCtrl", 100: "AltGr", 125: "Meta", 126: "RightMeta",
        272: "BTN_LEFT", 273: "BTN_RIGHT", 274: "BTN_MIDDLE", 275: "BTN_SIDE", 276: "BTN_EXTRA",
        277: "BTN_FORWARD", 278: "BTN_BACK"}
for row, first in (("QWERTYUIOP", 16), ("ASDFGHJKL", 30), ("ZXCVBNM", 44), ("1234567890", 2)):
    KEYS.update({first + i: c for i, c in enumerate(row)})


def key_name(code):
    return "+".join(KEYS.get(int(c), c) if c.isdigit() else c for c in str(code).split("+"))


def check_settings(conf):
    section("settings and mappings")
    gaze = {k: v for k, v in conf.items() if k.startswith(("POINTER_GAZE", "GAZE_", "POINTER_KEY_TAP",
                                                             "POINTER_HEAD_DEADZONE", "POINTER_HANDS"))}
    say("frametop.conf, gaze lines: " + (", ".join(f"{k}={v}" for k, v in gaze.items()) or "none (all defaults)"))
    try:
        rules = json.loads(INPUT.read_text())
    except (OSError, ValueError):
        say(f"{INPUT}: none, so the defaults (Meta+J gaze_left, Meta+K gaze_right)")
        return
    bindings = rules.get("key_bindings")
    if bindings is None:
        say("key combinations: the defaults (Meta+J gaze_left, Meta+K gaze_right)")
    else:
        mine = {key_name(k): v for k, v in bindings.items() if str(v).startswith("gaze")}
        say("key combinations for gaze: " + (", ".join(f"{k} {v}" for k, v in mine.items()) or "none"))
        if "gaze_left" not in bindings.values():
            say("note: no key combination is Gaze left click, so in the panel only a mouse click takes a dot")
    for dev, buttons in (rules.get("buttons") or {}).items():
        mine = {key_name(k): v for k, v in buttons.items() if str(v).startswith("gaze")}
        if mine:
            say(f"mouse {dev}: " + ", ".join(f"{k} {v}" for k, v in mine.items()))


# --- Logs ---

# What in the gaze service's log since it started says why it doesn't work. Exit 0 is SteamVR
# quitting, -15 a stop, 141 (SIGPIPE) the panel's at a stop.
LOG_PROBLEMS = (
    (r"ft-gaze stopped \(exit (?!(0|-15)\))", "ft-gaze (it reads the eye trackers) stopped with an error"),
    (r"ft-eyes stopped \(exit (?!(0|-15)\))", "ft-eyes (our eye tracker) stopped with an error"),
    (r"ft-gazepanel stopped \(exit (?!(0|-15|141)\))", "the calibration panel stopped with an error"),
    (r"isn't built", "a program isn't built"),
    (r"ft-gaze: SteamVR: ", "ft-gaze couldn't connect to SteamVR"),
    (r"layout is not recognized", "SteamVR's eye data has a layout ft-gaze doesn't know"),
    (r"Traceback|Error: ", "a Python error"),
    (r"can't bind @ft_gazed", "a second gaze service was started"),
    (r"the calibration can't open", "the calibration couldn't open"),
    (r"calibration failed|calibration not started", "a calibration failed"),
)


def logs():
    lines = journal("--user", "-u", UNIT, drop=r"^podman", last=0)
    start = max((i for i, line in enumerate(lines) if "Started Frametop gaze service" in line), default=0)
    for pattern, what in LOG_PROBLEMS:
        hits = [line for line in lines[start:] if re.search(pattern, line)]
        if hits:
            find(f"Since the gaze service started, {what}" + (f" ({len(hits)} times)" if len(hits) > 1 else "")
                 + f"; the last: {hits[-1]}")
    section("service log (this boot, folded, last 100; podman's left out)")
    say(*lines[-100:])
    section("pointer helper, gaze lines (last 20)")
    say(*journal("--user", "-u", "frametop-pointer", grep=r"gaze|calaccept|calquit|recheck|lesson", last=20))
    section("input relay, gaze lines (last 10)")
    say(*journal("--user", "-u", "frametop-input-relay", grep=r"gaze", last=10))
    section("frame grabber (frametop-eyegrab, last 20)")
    say(*journal("-u", "frametop-eyegrab", last=20))
    section("SteamVR server: the gaze service's programs and panel (last 20)")
    entries = steam_log(LOGS / "vrserver.txt") or []
    # Every OpenVR client gets the binding loads; ft-gaze binds only the headset (its eyetracking action).
    noise = (r"\[Workshop\]|attempting to load default config"
             r"|\((frame_controller|ft_pointer)\) has no configured binding")
    say(*fold([e for e in entries if re.search(r"ft-gaze|gazepanel", e[0])
               and re.search(r"New Connect|disconnected|SetActionManifestPath|\[(Error|Warning)\]", e[1])
               and not re.search(noise, e[0])])[-20:] or ["(nothing)"])


def main():
    args = sys.argv[1:]
    if args not in ([], ["--no-wake"]):
        sys.exit("usage: gaze-report.py [--no-wake]")
    repo, props = check_install()
    conf, setting, installed, tracker = gaze_settings(repo)
    worn, _ = check_service(props, wake=not args)
    check_steam_tracker(worn, repo)
    check_own(repo, tracker, setting, installed)
    check_history()
    check_settings(conf)
    logs()
    text = "\n".join(["===== Gaze: findings"] + [f"- {f}" for f in findings or
                                                   ["Nothing obviously wrong (the details below may still show it)"]]
                     + out)
    # As scripts/report.sh does: Bluetooth addresses and the headset's serial number.
    text = re.sub(r"([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", "xx:xx:xx:xx:xx:xx", text)
    print(re.sub(r"cv\.[A-Z0-9]{8,}", "cv.<serial>", text))


if __name__ == "__main__":
    main()
