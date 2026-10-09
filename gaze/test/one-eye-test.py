#!/usr/bin/env python3
"""Offline test of Track Dominant Eye Only (SteamOS 0.4): SteamVR's tracker ignores one eye,
and the gaze code then goes by the other (gazecal.tracked_eye, steady_samples' `eye`,
fitcheck's `ignore`, ft-gazed's live gaze). Reads only the temporary settings files it writes;
ft-gazed's Service is built without its sockets, so nothing reaches the live gaze service.

  gaze/test/one-eye-test.py
"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile
from collections import deque

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from fitcheck import FitCheck  # noqa: E402
from gazecal import (  # noqa: E402
    Correction,
    EyeFallback,
    Fixation,
    TrackedEye,
    steady_samples,
    tracked_eye,
)

loader = importlib.machinery.SourceFileLoader("ftgazed", os.path.join(HERE, "..", "ft-gazed"))
gazed = importlib.util.module_from_spec(importlib.util.spec_from_loader("ftgazed", loader))
loader.exec_module(gazed)

failures = []


def check(what, got, want):
    if got != want:
        failures.append(what)
        print(f"FAIL {what}: got {got!r}, want {want!r}", flush=True)


tmp = tempfile.mkdtemp(prefix="ft-one-eye-test-")
first, second = os.path.join(tmp, "a.vrsettings"), os.path.join(tmp, "b.vrsettings")
paths = (first, second)


def write(path, steamvr):
    with open(path, "w") as f:
        f.write(steamvr if isinstance(steamvr, str) else json.dumps({"steamvr": steamvr}, indent=3))


# --- Reading SteamVR's settings ---
check("no settings file: both eyes", tracked_eye(paths), None)
write(second, {"eyeTrackingDominantEyeOnly": True})
check("only the second file: it counts (right, SteamVR's default eye)", tracked_eye(paths), 1)
write(first, {"supersampleScale": 1.0})
check("the first file counts, without the setting: both eyes", tracked_eye(paths), None)
write(first, {"eyeTrackingDominantEyeOnly": True, "dominantEye": 0})
check("dominant eye left", tracked_eye(paths), 0)
write(first, {"eyeTrackingDominantEyeOnly": False, "dominantEye": 0})
check("setting off", tracked_eye(paths), None)
write(first, "{ not json")
check("a broken file: both eyes", tracked_eye(paths), None)

eye = TrackedEye(paths)
write(first, {"eyeTrackingDominantEyeOnly": True, "dominantEye": 1})
check("TrackedEye reads it", eye(now=100.0), 1)
write(first, {"eyeTrackingDominantEyeOnly": True, "dominantEye": 0, "pad": "x" * 10})
check("TrackedEye waits CHECK seconds", eye(now=101.0), 1)
check("then sees the change", eye(now=102.5), 0)

# --- One look at a dot: the left eye lost all along (what SteamVR's tracker may report for
# the eye it ignores), the right seen, and the vergence jumping with the lost eye ---
look = [{"t": i / 90, "src": {"mmap1": {"hy": 1.0, "hp": 2.0, "unc": [0.02, 0.001], "open": [0.0, 0.8],
                                        "lr": 2.8 if i % 2 else 9.0}}} for i in range(40)]
why = {}
check("both eyes judged: nothing kept", len(steady_samples(look, why=why)), 0)
check("both eyes judged: why", why, {"lost_left": 40})
why = {}
check("right eye only: all kept", len(steady_samples(look, why=why, eye=1)), 40)
check("right eye only: nothing dropped", why, {})
why = {}
check("left eye only: nothing kept", len(steady_samples(look, why=why, eye=0)), 0)
blink = [dict(s, src={"mmap1": dict(s["src"]["mmap1"], open=[0.0, 0.05])}) if 10 <= i < 15 else s
         for i, s in enumerate(look)]
why = {}
check("right eye only: its blinks still drop", len(steady_samples(blink, why=why, eye=1)), 35)
check("right eye only: as blinks", why, {"blink": 5})

# --- The headset fit check ---
fit = FitCheck(ignore=0)
for i in range(400):
    fit.feed({"src": {"mmap1": {"hy": 0.0, "hp": 0.0, "unc": [0.02, 0.001], "open": [0.0, 0.8]}}}, i / 90)
check("fit: the ignored eye's card", fit.status(0)[0], "not tracked")
check("fit: the tracked eye's card", fit.status(1)[0], "tracking")
hints = fit.hints()
check("fit: says which eye counts", hints[0].startswith("SteamVR tracks only your right eye"), True)
check("fit: no losses blamed on the ignored eye", any("Left eye: lost" in h for h in hints), False)
check("fit: the tracked eye is fine", hints[-1], "Your right eye is tracked everywhere you've looked so far.")
both = FitCheck()
for i in range(400):
    both.feed({"src": {"mmap1": {"hy": 0.0, "hp": 0.0, "unc": [0.02, 0.001], "open": [0.0, 0.8]}}}, i / 90)
check("fit, both eyes judged: the left eye is lost", both.status(0)[0], "LOST")

# --- ft-gazed's live gaze: the parts of Service the samples go through, no sockets ---
def service(one, source="mmap1"):
    svc = gazed.Service.__new__(gazed.Service)
    svc.override, svc.tracker, svc.source, svc.one_eye = None, "steam", source, one
    svc.models = {name: Correction() for name in gazed.SOURCES}
    svc.counts = dict.fromkeys(("samples", "sent", "blinks", "one_eye", "one_eye_used", "lost_left", "lost_right",
                                "looking_down", "dropped"), 0)
    svc.opens, svc.vergence = (deque(maxlen=90), deque(maxlen=90)), deque(maxlen=90)
    svc.lost, svc.bad_at, svc.fallback = [False, False], [0.0, 0.0], EyeFallback()
    svc.fix, svc.last_sample = Fixation(radius=1.0), 0.0
    svc.correction = lambda name, hy, hp: (0.0, 0.0)
    svc.sent = []
    svc.send = lambda t, hy, hp, rhy, rhp, eyes: svc.sent.append((hy, hp))
    return svc


def feed(svc, n=90):
    for i in range(n):
        svc.on_source_sample({"t": i / 90, "src": {
            "mmap1": {"hy": 1.0, "hp": 2.0, "unc": [0.02, 0.001], "open": [0.0, 0.8]},
            "mmap2": {"hy": 3.0, "hp": 2.0, "eyes": [[9.0, 9.0], [5.0, 2.0]]}}})


svc = service(None, "mmap2")
feed(svc)
check("mmap2, both eyes judged: a lost eye and no fallback yet drop the gaze", svc.sent, [])
svc = service(1, "mmap2")
feed(svc)
check("mmap2, right eye only: its own reading goes out", (len(svc.sent), svc.sent[-1] if svc.sent else None), (90, (5.0, 2.0)))
check("mmap2, right eye only: no blinks counted", svc.counts["blinks"], 0)
svc = service(0, "mmap1")
feed(svc)
check("left eye only, and it's lost: nothing goes out", (svc.sent, svc.counts["blinks"]), ([], 90))
svc = service(1)
check("no calibration: the source, as a whole", svc.kind, "source")
svc.models["right"].samples = 9
check("right eye only and calibrated: each eye's own", svc.kind, "eyes")
svc.one_eye = None
check("both eyes judged: the left needs a calibration too", svc.kind, "source")

for p in paths:
    if os.path.exists(p):
        os.remove(p)
os.rmdir(tmp)
print("FAILED: " + ", ".join(failures) if failures else "all passed")
sys.exit(1 if failures else 0)
