#!/usr/bin/env python3
"""Offline test of our own eye tracker's first calibration (gaze/gazecheck.py, "blind"): before
it has a calibration, ft-eyes publishes no gaze, and the calibration must still open and take
its dots. Until 2026-10-05 it couldn't: a fresh install that chose our tracker never calibrated.

Runs ft-gazed's Service with its sockets renamed and HOME in a temp folder (state and settings
go there), a fake pointer helper (gaze mode on, headset worn), a fake ft-gaze (SteamVR sees both
eyes; "own" is {"ok":0}, as with an uncalibrated ft-eyes), a fake ft-eyes control socket, and no
panel (a stand-in process). The fake ft-eyes also answers late or not at all, as a slow one
does: the service must keep running meanwhile (until 2026-10-09 it waited, up to 3 s a dot and
10 s for the fit, and the helper's 3 s panel lease ran out). Nothing reaches the live gaze service, the pointer helper, ft-eyes,
or SteamVR, so it's safe next to them.

  gaze/test/first-calibration-test.py
"""
import os
import tempfile

HOME = tempfile.mkdtemp(prefix="ft-gaze-first-cal-test-")
os.environ["HOME"] = HOME  # before gazecal: its STATE, and frametop.conf, follow HOME

import importlib.machinery  # noqa: E402
import importlib.util  # noqa: E402
import json  # noqa: E402
import selectors  # noqa: E402
import shutil  # noqa: E402
import socket  # noqa: E402
import subprocess  # noqa: E402
import sys  # noqa: E402
import threading  # noqa: E402
import time  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
GAZE = os.path.join(HERE, "..")
sys.path.insert(0, GAZE)
loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(GAZE, "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)
import gazecheck  # noqa: E402  (the module ft-gazed imported)

tag = f"ft_gaze_first_cal_test_{os.getpid()}"
gazed.ME = f"\0{tag}_gazed"
gazed.POINTER = gazecheck.POINTER = f"\0{tag}_helper"
gazecheck.SCREENS = f"\0{tag}_screens"
gazecheck.PANEL = f"\0{tag}_panel"
gazed.EYES_SOCKET = gazecheck.EYES = f"\0{tag}_eyes"
gazed.read_settings = lambda: ("own", "auto", "auto", 55.0)
gazed.TrackedEye = lambda: lambda now=None: None  # both eyes, whatever SteamVR's settings say
DOTS = 4
real_dots = gazecheck.check_dots
gazecheck.check_dots = lambda kind, own: real_dots(kind, own)[:DOTS]  # a short calibration
logs = []
gazed.log = gazecheck.log = lambda msg: logs.append(msg)

# The fake ft-gaze: 90 samples a second, SteamVR sees both eyes, no gaze from ours.
FAKE = os.path.join(HOME, "ft-gaze")
with open(FAKE, "w") as f:
    f.write('''import json, os, select, sys, time
while True:
    if select.select([sys.stdin], [], [], 1 / 90)[0]:
        if not os.read(0, 4096):
            break
    print(json.dumps({"t": time.monotonic(), "src": {"mmap1": {"hy": 1.0, "hp": 2.0, "unc": [0.001, 0.001],
          "open": [0.8, 0.8]}, "own": {"ok": 0}}}), flush=True)
''')
SLEEPER = [sys.executable, "-c", "import sys; sys.stdin.read()"]  # quits when its stdin closes


def start_helper(self):
    """ft-gaze, straight from here instead of the dev container."""
    self.proc = subprocess.Popen([sys.executable, FAKE], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
    self.proc_sources = self.wanted_sources()
    os.set_blocking(self.proc.stdout.fileno(), False)
    os.set_blocking(self.proc.stderr.fileno(), False)
    self.sel.register(self.proc.stdout, selectors.EVENT_READ, "stdout")
    self.sel.register(self.proc.stderr, selectors.EVENT_READ, "stderr")
    self.buf = b""


def start_eyes(self):
    """ft-eyes' process: a stand-in. Its control socket is the fake below."""
    self.eyes_proc = subprocess.Popen(SLEEPER, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(self.eyes_proc.stderr.fileno(), False)
    self.sel.register(self.eyes_proc.stderr, selectors.EVENT_READ, "eyes")


def start_panel(self):
    self.panel_proc = subprocess.Popen(SLEEPER, stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    os.set_blocking(self.panel_proc.stderr.fileno(), False)
    self.sel.register(self.panel_proc.stderr, selectors.EVENT_READ, "panel")


gazed.Service.start_helper = start_helper
gazed.Service.start_eyes = start_eyes
gazecheck.Checks.start_panel = start_panel

# The fake ft-eyes: uncalibrated until calib-fit. "fail" answers the next calib-point with that;
# "delay" holds calib-point's and calib-fit's answers that many seconds; "drop" leaves the next
# calib-point unanswered.
eyes_state = {"cal": None, "points": [], "fail": None, "delay": 0.0, "drop": False}
eyes = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
eyes.bind(gazed.EYES_SOCKET)
eyes.settimeout(0.2)


def eyes_answer():
    while True:
        try:
            data, addr = eyes.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            return
        w = data.decode().split()
        if w[0] == "status":
            reply = json.dumps({"calibration": eyes_state["cal"], "calibrating": False, "dots": 0,
                                "eyes": {"right": {"reseat": False}, "left": {"reseat": False}}})
        elif w[0] == "calib-start":
            eyes_state["points"] = []
            reply = "ok"
        elif w[0] == "calib-point":
            eyes_state["points"].append(tuple(map(float, w[1:5])))
            if eyes_state["drop"]:
                eyes_state["drop"] = False
                continue
            reply, eyes_state["fail"] = eyes_state["fail"] or "ok 50 50 1.00 1.00", None
        elif w[0] == "calib-fit":
            eyes_state["cal"] = {"made": "test", "dots": len(eyes_state["points"])}
            reply = f"ok {len(eyes_state['points'])} dots"
        else:
            reply = f"fail unknown command {w[0]}"
        if addr and w[0] in ("calib-point", "calib-fit") and eyes_state["delay"]:
            threading.Timer(eyes_state["delay"], send_late, (reply, addr)).start()
        elif addr:
            eyes.sendto(reply.encode(), addr)


def send_late(reply, addr):
    try:
        eyes.sendto(reply.encode(), addr)
    except OSError:
        pass  # the service gave up on it


threading.Thread(target=eyes_answer, daemon=True).start()

helper_state = {"reply": "ok off worn", "heard": [], "calpanel": []}  # calpanel: when "calpanel 1" came
helper = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
helper.bind(gazed.POINTER)
helper.settimeout(0.2)


def helper_answer():
    while True:
        try:
            data, addr = helper.recvfrom(512)
        except socket.timeout:
            continue
        except OSError:
            return
        if data.startswith(b"gaze ?") and addr:
            helper.sendto(helper_state["reply"].encode(), addr)
        else:
            helper_state["heard"].append(data.decode())
            if data == b"calpanel 1":
                helper_state["calpanel"].append(time.monotonic())


threading.Thread(target=helper_answer, daemon=True).start()
svc = gazed.Service(None, False, gazed.POINTER)
threading.Thread(target=svc.run, daemon=True).start()
ctl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
ctl.bind("")
ctl.settimeout(3)


def ask(cmd):
    ctl.sendto(cmd.encode(), gazed.ME)
    return ctl.recv(65536).decode()


failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def wait(cond, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if cond():
            return True
        time.sleep(0.05)
    return cond()


def check_state():
    return svc.checks.check or {}


def take_dot(i):
    """Wait for dot i to settle, then click: is it taken?"""
    wait(lambda: check_state().get("i") == i and not check_state().get("done_at"), 3)
    time.sleep(gazecheck.CHECK_SETTLE + gazecheck.CHECK_WINDOW + 0.1)
    before = check_state().get("captured", 0)
    ask("calaccept")
    return wait(lambda: check_state().get("captured", 0) > before, 2)


check("gaze mode off, Gaze page open (wake): ours runs, uncalibrated", ask("wake 60"), "ok")
check("the service knows ours has no calibration", wait(lambda: svc.checks.calibrated() is False, 6), True)
# ft-eyes' status can come before the fake ft-gaze's first sample, and without one the refusal
# below is "the headset is off" instead (failed about 1 run in 4 until 2026-10-06).
check("SteamVR sees the eyes (the fake ft-gaze is sending)", wait(svc.checks.eyes_seen, 6), True)
check("a quick check is refused while ours has no calibration", svc.checks.start("quick", "test"),
      "error our tracker isn't calibrated yet: use Calibrate")
helper_state["reply"] = "ok on worn"
check("gaze mode on: the calibration opens by itself", wait(lambda: check_state().get("kind") == "full", 6), True)
check("it runs blind (no gaze from ours yet)", check_state().get("blind"), True)
check("nothing says it can't open", any("can't open" in m for m in logs), False)
# Live 2026-10-05: turning gaze mode on woke our tracker, and the calibration opened before
# DON_DELAY of eyes had passed, so "the headset went on" came while it was open. That re-armed
# it, and a second calibration opened as soon as the first ended.
svc.checks.away, svc.checks.back_since = True, None

check("dot 1: a click takes it", take_dot(0), True)
check("the headset went on while it was open",
      wait(lambda: not svc.checks.away, gazecheck.DON_DELAY + 2) and bool(svc.checks.check), True)
t0, t1, yaw, pitch = eyes_state["points"][0]
dot = gazecheck.check_dots("full", True)[0]
check("its window is the look up to the click (about CHECK_WINDOW)",
      abs((t1 - t0) - gazecheck.CHECK_WINDOW) < 0.15, True)
check("ft-eyes got that dot's direction", (round(yaw, 3), round(pitch, 3)), (round(dot[0], 3), round(dot[1], 3)))

eyes_state["fail"] = "fail the left eye was seen in only 3 frames"
wait(lambda: check_state().get("i") == 1 and not check_state().get("done_at"), 3)
time.sleep(gazecheck.CHECK_SETTLE + gazecheck.CHECK_WINDOW + 0.1)
ask("calaccept")
check("ft-eyes refusing a dot: its reason reaches the panel's note",
      wait(lambda: "left eye in only 3 frames" in check_state().get("note", ""), 2), True)
check("dot 2, second try: taken", take_dot(1), True)

# A slow ft-eyes (2.5 s): the service goes on meanwhile, and a second click is ignored.
eyes_state["delay"] = 2.5
wait(lambda: check_state().get("i") == 2 and not check_state().get("done_at"), 3)
time.sleep(gazecheck.CHECK_SETTLE + gazecheck.CHECK_WINDOW + 0.1)
before = len(eyes_state["points"])
ask("calaccept")
check("dot 3, ft-eyes slow: the service waits for it", wait(lambda: svc.checks.asking is not None, 2), True)
t = time.monotonic()
ask("status")
check("the service still answers meanwhile", time.monotonic() - t < 0.5, True)
ask("calaccept")
check("dot 3: taken once ft-eyes answers", wait(lambda: check_state().get("captured") == 3, 4), True)
check("the helper's panel lease was renewed while ft-eyes took its time",
      sum(t < at < t + eyes_state["delay"] for at in helper_state["calpanel"]) >= 2, True)
check("and the calibration is still open (a blocked service took that as the headset off)",
      check_state().get("kind"), "full")
check("the second click asked ft-eyes nothing", len(eyes_state["points"]), before + 1)
eyes_state["delay"] = 0.0

# No answer: the dot isn't taken, after calib-point's 3 s.
eyes_state["drop"] = True
wait(lambda: check_state().get("i") == 3 and not check_state().get("done_at"), 3)
time.sleep(gazecheck.CHECK_SETTLE + gazecheck.CHECK_WINDOW + 0.1)
ask("calaccept")
check("dot 4, no answer from ft-eyes: not taken, and the panel says so",
      wait(lambda: "didn't answer" in check_state().get("note", ""), 5), True)
eyes_state["delay"] = 2.5  # the fit too
check("dot 4, second try: taken", take_dot(3) or wait(lambda: check_state().get("captured") == 4, 4), True)

check("while ours fits, the panel stays", wait(lambda: check_state().get("fitting") is True, 3), True)
ask("calquit")
check("and a right click doesn't close it", check_state().get("kind"), "full")
check("all dots: ours fits its calibration (calib-fit)",
      wait(lambda: eyes_state["cal"] is not None and not svc.checks.check, 5), True)
gaps = [b - a for a, b in zip(helper_state["calpanel"], helper_state["calpanel"][1:])]
check("the helper's panel lease (3 s) never ran out", bool(gaps) and max(gaps) < 3.0, True)
check("the service sees it calibrated", wait(lambda: svc.checks.calibrated() is True, 4), True)
check("and gaze mode stays on", "gaze off" in helper_state["heard"], False)
check("and no second calibration opens", wait(lambda: svc.checks.check is not None, 3), False)
check("one calibration in the log", sum(m.startswith("full check:") for m in logs), 1)

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
svc.running = False
time.sleep(0.7)
shutil.rmtree(HOME, ignore_errors=True)
os._exit(1 if failures else 0)
