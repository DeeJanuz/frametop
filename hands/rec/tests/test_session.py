#!/usr/bin/env python3
"""Tests for session.py's step mode (dry runs, no processes): the ready, countdown and hold
timeline in prompts.jsonl, R (redo), the timed flow (--auto), the pose pictures' display rule,
the length estimates, and a take recorded in many parts through review, export and validate.

  python3 hands/rec/tests/test_session.py
"""
import json
import os
import shutil
import sys
import tempfile
import struct
import time
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import session  # noqa: E402
import takes  # noqa: E402
import validate  # noqa: E402
from test_validate import fhset, make_session, SESSION  # noqa: E402

SCRIPT = {
    "version": 1, "intro_s": 1, "between_s": 1,
    "welcome": {"title": "Test", "seconds": 1, "text": "A test."},
    "done": {"title": "Done", "seconds": 0, "text": "Done."},
    "stopped": {"title": "Stopped", "seconds": 0, "text": "Stopped."},
    "sections": [
        {"id": "poses", "title": "Poses", "intro": "Some poses.", "go": "Hold",
         "prompts": [{"text": "Fist.", "seconds": 4, "hands": "left", "pose": "fist", "distance": "near",
                      "position": "left"},
                     {"text": "Open.", "seconds": 4, "hands": "both", "pose": "open"}]}]}


class SessionBase(unittest.TestCase):
    """Dry-run sessions of SCRIPT, steered from the test."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="handrec-session-test-")
        self.script = os.path.join(self.tmp, "script.json")
        with open(self.script, "w") as f:
            json.dump(SCRIPT, f)
        self.panel = []

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def session(self, **kw):
        kw.setdefault("speed", 10)
        s = session.Session(os.path.join(self.tmp, "base"), {}, {}, "room", self.script, dry_run=True,
                            poses_dir=os.path.join(self.tmp, "no-poses"), **kw)
        s.print = self.panel.append
        return s

    def wait_for(self, s, pred, what, timeout=10):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            st = dict(s._status)
            if pred(st):
                return st
            time.sleep(0.005)
        self.fail("timed out waiting for %s: %s" % (what, s._status))

    def waiting(self, s, state, prompt=None):
        return self.wait_for(s, lambda st: st["state"] == state and st["waiting"]
                             and (prompt is None or st["prompt"] == prompt), "%s %s" % (state, prompt or ""))

    def events(self, s):
        with open(os.path.join(s.session_dir, "takes", "01-poses", "prompts.jsonl")) as f:
            return [json.loads(line) for line in f]



class SessionTest(SessionBase):
    def test_step_flow(self):
        s = self.session(next_after=0.0)
        s.start()
        s.join(20)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        self.assertEqual([e["event"] for e in ev],
                         ["take", "ready", "prompt", "wait", "ready", "prompt", "wait", "end"])
        self.assertEqual(ev[1]["id"], ev[2]["id"])
        self.assertEqual(ev[1]["seconds"], session.COUNTDOWN_S)
        self.assertTrue(ev[1]["t"] < ev[2]["t"] < ev[3]["t"] < ev[4]["t"])
        # the countdown is recorded (3 s at 10x speed), and the hold after it
        self.assertGreater(ev[2]["t"] - ev[1]["t"], 0.25e9)
        with open(os.path.join(s.session_dir, "session.json")) as f:
            self.assertEqual(json.load(f)["mode"], "step")
        # a clock sample as each part starts (2 steps: 2 parts), for capture_ns (RAW) -> MONOTONIC
        with open(os.path.join(s.session_dir, "takes", "01-poses", "take.json")) as f:
            clock = json.load(f)["clock"]
        self.assertEqual(len(clock), 2)
        self.assertTrue(all(len(c) == 2 and all(isinstance(v, int) for v in c) for c in clock))
        self.assertLess(clock[0][0], clock[1][0])
        # the panel: Ready?, then the countdown, then the section's word for the hold, the diagram
        self.assertIn("panel: action " + session.READY_TEXT, self.panel)
        self.assertLess(self.panel.index("panel: big 3"), self.panel.index("panel: big 1"))
        self.assertIn("panel: big Hold", self.panel)
        self.assertIn("panel: where left near", self.panel)
        self.assertIn("panel: rec on", self.panel)

    def test_redo(self):
        s = self.session()
        s.start()
        self.waiting(s, "starting")
        s.next_step()
        self.waiting(s, "intro")
        s.redo()   # nothing to do again yet: dropped
        s.next_step()
        st = self.waiting(s, "ready", "Fist.")
        self.assertFalse(st["can_redo"])
        s.next_step()
        self.wait_for(s, lambda st: st["state"] == "running", "the hold")
        s.redo()   # during the hold: it starts again
        self.waiting(s, "ready", "Fist.")
        s.next_step()
        st = self.waiting(s, "ready", "Open.")
        self.assertTrue(st["can_redo"])
        s.redo()   # at the next step's ready screen: the one before goes again
        self.waiting(s, "ready", "Fist.")
        s.next_step()
        self.waiting(s, "ready", "Open.")
        s.next_step()
        s.join(20)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        names = [e["event"] for e in ev]
        self.assertEqual(names, ["take", "ready", "prompt", "redo", "wait", "ready", "prompt", "wait", "redo",
                                 "ready", "prompt", "wait", "ready", "prompt", "wait", "end"])
        first, second = ev[3], ev[8]
        self.assertEqual(first["id"], "poses/fist/left/near/left")
        self.assertEqual(first["from"], ev[1]["t"])          # from its countdown
        self.assertTrue(ev[2]["t"] < first["to"] <= ev[4]["t"])
        self.assertEqual((second["from"], second["id"]), (ev[5]["t"], first["id"]))
        self.assertTrue(ev[6]["t"] < second["to"] <= ev[7]["t"])
        # what labels keep: prompts outside every redo range
        kept = [e["id"] for e in ev if e["event"] == "prompt"
                and not any(r["from"] <= e["t"] <= r["to"] for r in ev if r["event"] == "redo")]
        self.assertEqual(kept, ["poses/fist/left/near/left", "poses/open/both"])

    def test_pause_while_waiting(self):
        s = self.session()
        s.start()
        self.waiting(s, "starting")
        s.next_step()
        self.waiting(s, "intro")
        s.next_step()
        self.waiting(s, "ready", "Fist.")
        s.pause()
        self.wait_for(s, lambda st: st["state"] == "paused", "paused")
        s.next_step()   # ignored while paused
        s.resume()
        st = self.waiting(s, "ready", "Fist.")
        s.stop(wait=10)
        self.assertEqual(s.state, "stopped")
        # nothing recorded: no take
        self.assertEqual(os.listdir(os.path.join(s.session_dir, "takes")), [])

    def test_auto(self):
        s = self.session(auto=True, speed=20)
        s.start()
        s.join(20)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        self.assertEqual([e["event"] for e in ev], ["take", "prompt", "prompt", "prompt", "end"])
        self.assertEqual(ev[1]["id"], "poses/intro")

    def test_auto_redo_after_pause(self):
        s = self.session(auto=True)
        s.start()
        self.wait_for(s, lambda st: st["state"] == "running" and st["prompt"] == "Fist.", "the first prompt")
        s.pause()
        self.wait_for(s, lambda st: st["state"] == "paused", "paused")
        s.redo()   # ends the pause; the prompt starts again, recording again
        s.join(20)
        self.assertEqual(s.state, "done")
        names = [e["event"] for e in self.events(s)]
        self.assertEqual(names, ["take", "prompt", "prompt", "pause", "redo", "resume", "prompt", "prompt", "end"])

    def test_pose_view(self):
        d = os.path.join(self.tmp, "poses")
        os.makedirs(d)
        for name in ("fist.png", "cross.png"):
            open(os.path.join(d, name), "wb").close()
        with open(os.path.join(d, "poses.json"), "w") as f:
            json.dump({"fist": {"file": "fist.png", "two_hands": False, "caption": "A fist"},
                       "cross": {"file": "../cross.png", "two_hands": True, "caption": ""},
                       "ok": {"file": "ok.png", "two_hands": False}}, f)
        poses = session.load_poses(d)
        fist = os.path.join(d, "fist.png")
        self.assertEqual(session.pose_view(poses, {"pose": "fist", "hands": "right"}), (fist, "", "A fist"))
        self.assertEqual(session.pose_view(poses, {"pose": "fist", "hands": "left"})[1], "mirror")
        self.assertEqual(session.pose_view(poses, {"pose": "fist", "hands": "both"})[1], "both")
        self.assertEqual(session.pose_view(poses, {"pose": "fist", "hands": "any"})[1], "")
        # two hands drawn already; the file stays in the folder
        self.assertEqual(session.pose_view(poses, {"pose": "cross", "hands": "both"})[:2],
                         (os.path.join(d, "cross.png"), ""))
        self.assertEqual(session.pose_view(poses, {"pose": "ok", "hands": "both"}), ("", "", ""))   # no file
        self.assertEqual(session.pose_view(poses, {"pose": "claw", "hands": "both"}), ("", "", ""))
        self.assertEqual(session.load_poses(os.path.join(self.tmp, "none")), {})

    def test_plan(self):
        plan, _ = session.build_plan(SCRIPT, {})
        self.assertEqual(session.plan_steps(plan), 2)
        self.assertEqual(session.plan_seconds(SCRIPT, plan, auto=False), 2 * session.COUNTDOWN_S + 8)
        self.assertEqual(session.plan_seconds(SCRIPT, plan, auto=True), 1 + 1 + 8)
        self.assertIn("2 steps", session.plan_summary(SCRIPT, plan))


SWEEP_SCRIPT = {
    "version": 1, "intro_s": 1, "between_s": 1,
    "welcome": {"title": "Test", "seconds": 1, "text": "A test."},
    "done": {"title": "Done", "seconds": 0, "text": "Done."},
    "stopped": {"title": "Stopped", "seconds": 0, "text": "Stopped."},
    "cue_names": {"thumbs-up": "Thumbs up"},
    "sections": [
        {"id": "sweeps", "title": "Poses", "kind": "sweep", "intro": "Keep moving.", "quick": True,
         "cue_s": 2, "step_s": 6,
         "groups": [["open", "fist"], ["ok", "thumbs-up", "claw"]],
         "sweeps": [{"hands": "both", "group": "next", "text": "Both hands."},
                    {"hands": "left", "group": "any", "quick": False, "text": "Left hand."}]}]}


class SweepTest(SessionBase):
    def setUp(self):
        super().setUp()
        with open(self.script, "w") as f:
            json.dump(SWEEP_SCRIPT, f)

    def events(self, s):
        with open(os.path.join(s.session_dir, "takes", "01-sweeps", "prompts.jsonl")) as f:
            return [json.loads(line) for line in f]

    def test_sweep_flow(self):
        s = self.session(next_after=0.0, speed=10, seed=1)
        s.start()
        s.join(20)
        self.assertEqual(s.state, "done")
        ev = self.events(s)
        names = [e["event"] for e in ev]
        self.assertEqual(names, ["take", "ready", "prompt", "prompt", "prompt", "wait",
                                 "ready", "prompt", "prompt", "prompt", "wait", "end"])
        plan = {p["id"]: p["cues"] for p in s.plan[0]["prompts"]}
        for step, first in (("sweeps/both-1", 2), ("sweeps/left-1", 7)):
            cues = [e for e in ev[first:first + 3]]
            self.assertEqual([e["pose"] for e in cues], plan[step])          # one prompt per cue, in order
            self.assertTrue(all(e["cue"] and e["step"] == step for e in cues))
            self.assertTrue(all(e["distance"] == e["position"] == "" for e in cues))
            self.assertEqual(ev[first - 1], dict(ev[first - 1], event="ready", id=step))
            gaps = [(b["t"] - a["t"]) / 1e9 for a, b in zip(cues, cues[1:])]
            self.assertTrue(all(0.15 < g < 0.4 for g in gaps), gaps)          # cue_s 2 at 10x speed
        # a cue repeated within a step (6 s of 2 s cues from a group of 2) gets its own id
        both = [e["id"] for e in ev[2:5]]
        self.assertEqual(len(set(both)), 3)
        # the strip: every picture of the step, the cue lit
        strips = [c for c in self.panel if c.startswith("panel: strip ")]
        self.assertIn("panel: strip off", strips)
        self.assertTrue(any(c.startswith("panel: strip 1 ") for c in strips))
        with open(os.path.join(s.session_dir, "session.json")) as f:
            meta = json.load(f)
        self.assertEqual(meta["shuffle"]["seed"], 1)
        self.assertEqual({x["id"]: x["cues"] for x in meta["shuffle"]["sweeps"]["sweeps"]}, plan)
        self.assertFalse(meta["quick"])

    def test_seed_from_session_id(self):
        s = self.session(next_after=0.0, speed=20)
        s.start()
        s.join(20)
        with open(os.path.join(s.session_dir, "session.json")) as f:
            meta = json.load(f)
        sid = os.path.basename(s.session_dir)
        self.assertEqual(meta["shuffle"]["seed"], session.session_seed(sid))
        again, _ = session.build_plan(SWEEP_SCRIPT, {}, seed=meta["shuffle"]["seed"])   # reproducible
        self.assertEqual(session.plan_record(again), meta["shuffle"]["sweeps"])

    def test_quick_session(self):
        s = self.session(next_after=0.0, speed=20, quick=True)
        s.start()
        s.join(20)
        self.assertEqual(s.state, "done")
        self.assertEqual([e["event"] for e in self.events(s)].count("ready"), 1)   # the left sweep left out
        with open(os.path.join(s.session_dir, "session.json")) as f:
            self.assertTrue(json.load(f)["quick"])

    def test_auto(self):
        s = self.session(auto=True, speed=20)
        s.start()
        s.join(20)
        self.assertEqual(s.state, "done")
        names = [e["event"] for e in self.events(s)]
        self.assertEqual(names, ["take", "prompt"] + ["prompt"] * 6 + ["end"])   # intro, 3 cues a step


class ShuffleTest(unittest.TestCase):
    def setUp(self):
        self.script = session.load_script(session.SCRIPT_PATH)

    def cues(self, seed, quick=False, checklist=None):
        plan, _ = session.build_plan(self.script, checklist or {}, seed=seed, quick=quick)
        return session.plan_record(plan)

    def test_deterministic(self):
        self.assertEqual(self.cues(42), self.cues(42))
        self.assertNotEqual(self.cues(42)["pose-sweeps"], self.cues(43)["pose-sweeps"])
        self.assertEqual(session.session_seed("20261002-202734"), session.session_seed("20261002-202734"))
        self.assertNotEqual(session.session_seed("20261002-202734"), session.session_seed("20261002-202735"))

    def test_groups(self):
        groups = [sorted(g) for g in self.script["sections"][1]["groups"]]
        for seed in range(20):
            steps = self.cues(seed)["pose-sweeps"]
            self.assertEqual([x["hands"] for x in steps], ["both", "both", "both", "left", "right"])
            used = [sorted(set(x["cues"])) for x in steps]
            self.assertEqual(sorted(used[:3]), sorted(groups))       # each group once with both hands
            self.assertTrue(all(u in groups for u in used[3:]))
            self.assertNotEqual(used[3], used[4])                    # the one-hand sweeps differ
            self.assertTrue(all(len(x["cues"]) == 5 for x in steps))  # 20 s of 4 s cues
        # unshuffled: the script's order
        steps = self.cues(None)["pose-sweeps"]
        self.assertEqual(steps[0]["cues"], ["open", "fist", "point", "pinch", "open"])
        # gestures and hand size keep their order
        for seed in (1, 2, 3):
            self.assertEqual([x["cues"] for x in self.cues(seed)["gestures"]],
                             [["pinch-tap", "pinch-drag"], ["grab", "cross", "overlap"], ["near-face", "screen-point"]])
            self.assertEqual(self.cues(seed)["hand-size"][0]["cues"], ["flat", "flat-back", "spread"])

    def test_plans(self):
        full, skipped = session.build_plan(self.script, {}, seed=5)
        self.assertEqual([s["id"] for s in full],
                         ["hand-size", "pose-sweeps", "gestures", "desk-work", "touch", "bare-push", "no-hands"])
        self.assertEqual(session.plan_steps(full), 15)
        self.assertLess(session.plan_seconds(self.script, full, auto=False), 6 * 60)
        everything, _ = session.build_plan(self.script, {"objects": ["pencil", "keyboard", "mouse"],
                                                         "controllers": "straps"}, seed=5)
        self.assertEqual(len(everything), 10)
        quick, skipped = session.build_plan(self.script, {"objects": ["pencil"], "controllers": "straps"},
                                            seed=5, quick=True)
        self.assertEqual([s["id"] for s in quick], ["hand-size", "pose-sweeps", "touch", "no-hands"])
        self.assertEqual([p["hands"] for p in quick[1]["prompts"]], ["both"] * 3)
        self.assertEqual(len(quick[2]["targets"]), 6)
        self.assertLess(session.plan_seconds(self.script, quick, auto=False), 150)
        self.assertIn({"section": "objects", "reason": "not in a quick round"}, skipped)
        # desk work: the mouse-and-keyboard step only with both ticked
        desk = [s for s in everything if s["id"] == "desk-work"][0]["prompts"]
        self.assertEqual([p.get("cues") for p in desk], [None, ["mouse", "switch"]])


class StripPanelTest(unittest.TestCase):
    """The panel's strip command on ft-handpanel --no-vr (skipped if it isn't built)."""

    def test_strip(self):
        import subprocess
        binary = session.PANEL_BIN
        if not os.access(binary, os.X_OK):
            self.skipTest("ft-handpanel isn't built")
        name = "hrtest-strip-%d" % os.getpid()
        proc = subprocess.Popen([binary, "--no-vr", "--socket", name], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True)
        try:
            panel = session.Panel(name=name)
            for _ in range(50):
                if panel.cmd("ping", reply=True, timeout=0.1):
                    break
                if proc.poll() is not None:
                    self.skipTest("ft-handpanel doesn't run here: " + proc.stderr.read().strip()[-200:])
                time.sleep(0.1)
            else:
                self.fail("ft-handpanel --no-vr doesn't answer")
            poses = session.POSES_DIR
            ok = panel.cmd("strip 1 %s/fist.png|mirror|Fist;-|-|No picture;%s/ok.png|-|OK" % (poses, poses), reply=True)
            bad = panel.cmd("strip 1", reply=True)
            missing = panel.cmd("strip 0 /nonexistent.png|-|Gone", reply=True)
            panel.cmd("show", reply=True)
            panel.cmd("strip off", reply=True)
            panel.close()
        finally:
            proc.terminate()
            out = proc.communicate(timeout=5)[0]
        self.assertEqual(ok, "ok")
        self.assertTrue(bad.startswith("error usage: strip"))
        self.assertTrue(missing.startswith("error can't read"))
        self.assertIn("strip: cue 0: Gone(/nonexistent.png)", out)
        self.assertIn("strip: off", out)


# /proc/bus/input/devices as the Frame has it (2026-10-02), trimmed, plus a USB mouse.
DEVICES = """I: Bus=0019 Vendor=0001 Product=0001 Version=0100
N: Name="gpio-keys"
P: Phys=gpio-keys/input0
S: Sysfs=/devices/platform/gpio-keys/input/input3
U: Uniq=
H: Handlers=kbd kbd event3 qcom_pon_combo_timer_listener 
B: PROP=0
B: EV=3
B: KEY=100000000000 0 0 0 0 200000000 0 0 0 0 0

I: Bus=0006 Vendor=4d44 Product=0001 Version=0001
N: Name="frametop virtual mouse"
P: Phys=
S: Sysfs=/devices/virtual/input/input4
U: Uniq=
H: Handlers=event4 qcom_pon_combo_timer_listener 
B: PROP=0
B: EV=7
B: KEY=ffffff 0 0 0 0
B: REL=ffff

I: Bus=0003 Vendor=0001 Product=0001 Version=0001
N: Name="frame-voice keyboard"
P: Phys=py-evdev-uinput
S: Sysfs=/devices/virtual/input/input7
U: Uniq=
H: Handlers=kbd sysrq kbd event7 qcom_pon_combo_timer_listener 
B: PROP=0
B: EV=200003
B: KEY=1ffffffffffffff ffffffffffffffff ffffffffffffffff fffffffffffffffe

I: Bus=0005 Vendor=214e Product=0035 Version=0001
N: Name="Z3 Keyboard"
P: Phys=90:82:c3:5f:4a:a7
S: Sysfs=/devices/virtual/misc/uhid/0005:214E:0035.0005/input/input27
U: Uniq=da:ef:2f:d2:be:e2
H: Handlers=kbd sysrq kbd event9 qcom_pon_combo_timer_listener 
B: PROP=0
B: EV=10001f
B: KEY=300000000000 33eff 0 0 483ffff17aff32d bfd4444600000000 1 130ff38b17c007 ffff7bfad9415fff feb2ffdfffefffff fffffffffffffffe
B: REL=1040
B: MSC=10
"""
BT_MOUSE = """
I: Bus=0005 Vendor=214e Product=0035 Version=0001
N: Name="Z3 Mouse"
P: Phys=90:82:c3:5f:4a:a7
S: Sysfs=/devices/virtual/misc/uhid/0005:214E:0035.0005/input/input26
U: Uniq=da:ef:2f:d2:be:e2
H: Handlers=event8 qcom_pon_combo_timer_listener 
B: PROP=0
B: EV=17
B: KEY=ffff0000 0 0 0 0
B: REL=1943
B: MSC=10
"""
USB_MOUSE = """
I: Bus=0003 Vendor=046d Product=c077 Version=0111
N: Name="Logitech USB Optical Mouse"
P: Phys=usb-xhci-hcd.1.auto-1/input0
S: Sysfs=/devices/platform/soc@0/a600000.usb/xhci-hcd.1.auto/usb1/1-1/1-1:1.0/0003:046D:C077.0001/input/input40
U: Uniq=
H: Handlers=mouse0 event13
B: PROP=0
B: EV=17
B: KEY=70000 0 0 0 0
B: REL=903
B: MSC=10
"""


def event(etype, code, value, t=1.000002):
    return session.INPUT_EVENT.pack(int(t), round(t % 1 * 1e6), etype, code, value)


def taps(*times):
    """Presses, each 50 ms down, at these times (the kernel's clock), as one write."""
    return b"".join(event(session.EV_KEY, session.KEY_SELECT, 1, t) + event(0, 0, 0, t)
                    + event(session.EV_KEY, session.KEY_SELECT, 0, t + 0.05) + event(0, 0, 0, t + 0.05)
                    for t in times)


PRESS = event(session.EV_KEY, session.KEY_SELECT, 1) + event(0, 0, 0)
RELEASE = event(session.EV_KEY, session.KEY_SELECT, 0) + event(0, 0, 0)


class InputTest(unittest.TestCase):
    def test_devices(self):
        devs = session.parse_input_devices(DEVICES)
        self.assertEqual([d["name"] for d in devs], ["gpio-keys", "frametop virtual mouse", "frame-voice keyboard",
                                                     "Z3 Keyboard"])
        self.assertEqual(session.find_button(devs), "/dev/input/event3")
        self.assertFalse(any(session.real_mouse(d) for d in devs))   # the virtual mouse, keyboards with wheels
        for extra, name in ((BT_MOUSE, "Z3 Mouse"), (USB_MOUSE, "Logitech USB Optical Mouse")):
            mice = [d["name"] for d in session.parse_input_devices(DEVICES + extra) if session.real_mouse(d)]
            self.assertEqual(mice, [name])
        # another gpio-keys without KEY_SELECT isn't the button
        self.assertIsNone(session.find_button(session.parse_input_devices(DEVICES.replace("200000000", "0"))))

    def test_events(self):
        data = (PRESS + event(session.EV_KEY, session.KEY_SELECT, 2) * 3   # autorepeat: left out
                + RELEASE + event(session.EV_KEY, 115, 1) + PRESS)        # another key
        self.assertEqual(session.button_events(data), ([(True, 1.000002), (False, 1.000002), (True, 1.000002)], b""))
        out, rest = session.button_events(PRESS + PRESS[:10])
        self.assertEqual((out, rest), ([(True, 1.000002)], PRESS[:10]))   # half an event waits for the rest
        self.assertEqual(session.button_events(rest + PRESS[10:])[0], [(True, 1.000002)])
        self.assertEqual([t for _, t in session.button_events(taps(5.25))[0]], [5.25, 5.3])

    def test_gestures(self):
        def run(steps, **kw):
            """steps: (time, "down" | "up" | "tick"). The gestures, with the time each came."""
            g, out = session.ButtonGestures(double_s=0.4, hold_s=1.5, bounce_s=0.05, **kw), []
            for t, what in steps:
                out += [(t, x) for x in getattr(g, what)(t)]
            return out
        # a press comes once the double-press time has passed
        self.assertEqual(run([(0, "down"), (0.1, "up"), (0.3, "tick"), (0.6, "tick")]), [(0.6, "press")])
        # two: a double, at the second down, and nothing at its release
        self.assertEqual(run([(0, "down"), (0.1, "up"), (0.4, "down"), (0.5, "up"), (2, "tick")]),
                         [(0.4, "double")])
        # the second too late: two presses (the first reported when the second goes down)
        self.assertEqual(run([(0, "down"), (0.1, "up"), (0.6, "down"), (0.7, "up"), (1.2, "tick")]),
                         [(0.6, "press"), (1.2, "press")])
        # a hold: while still down, and nothing at the release
        self.assertEqual(run([(0, "down"), (1.0, "tick"), (1.5, "tick"), (3, "up"), (4, "tick")]),
                         [(1.5, "hold")])
        # a hold noticed only at the release (a slow read) still counts
        self.assertEqual(run([(0, "down"), (2, "up"), (3, "tick")]), [(2, "hold")])
        # a bounce (up and down 20 ms apart) is one press
        self.assertEqual(run([(0, "down"), (0.1, "up"), (0.12, "down"), (0.2, "up"), (1, "tick")]),
                         [(1, "press")])
        # a press, then a hold soon after: a double (the hold counts from a fresh down)
        self.assertEqual(run([(0, "down"), (0.1, "up"), (0.3, "down"), (2, "tick"), (2.1, "up")]),
                         [(0.3, "double")])
        # a release with no down, and two downs: left alone
        self.assertEqual(run([(0, "up"), (1, "down"), (1.1, "down"), (1.2, "up"), (2, "tick")]),
                         [(2, "press")])

    def test_reader_fifo(self):
        tmp = tempfile.mkdtemp(prefix="handrec-button-test-")
        try:
            fifo = os.path.join(tmp, "button")
            os.mkfifo(fifo)
            got = []
            reader = session.ButtonReader(fifo, got.append, double_s=0.15, hold_s=0.5).start()
            with open(fifo, "wb", buffering=0) as w:
                w.write(PRESS + RELEASE)                 # a press
                time.sleep(0.3)
                w.write(PRESS[:7])                       # two, the first split across writes
                time.sleep(0.02)
                w.write(PRESS[7:] + RELEASE)
                time.sleep(0.07)
                w.write(PRESS + RELEASE)
                time.sleep(0.3)
                w.write(taps(10, 10.1))                  # two in one read: the kernel's times tell them apart
                time.sleep(0.3)
                w.write(PRESS)                           # held
                time.sleep(0.7)
                self.assertEqual(got, ["press", "double", "double", "hold"])   # the hold came before the release
                w.write(RELEASE)
                time.sleep(0.2)
            with open(fifo, "wb", buffering=0) as w:     # the writer comes back: read again
                time.sleep(0.4)
                w.write(PRESS + RELEASE)
                time.sleep(0.3)
            reader.stop()
            self.assertEqual(got, ["press", "double", "double", "hold", "press"])
            self.assertTrue(reader.ok)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

    def test_missing_device(self):
        logs = []
        reader = session.ButtonReader("/nonexistent/event99", lambda: None, log=logs.append).start()
        time.sleep(0.1)
        reader.stop()
        self.assertFalse(reader.ok)
        self.assertIn("can't open", logs[0])


class ButtonSessionTest(SessionBase):
    """A dry run steered with a simulated headset button (a FIFO), with no mouse, then one."""

    def test_button(self):
        fifo = os.path.join(self.tmp, "button")
        os.mkfifo(fifo)
        procfile = os.path.join(self.tmp, "devices")
        with open(procfile, "w") as f:
            f.write(DEVICES)
        writer = os.open(fifo, os.O_RDWR)   # kept open, so the reader never sees the end
        press = lambda: os.write(writer, PRESS + RELEASE)
        try:
            with mock.patch.object(session, "BUTTON_DOUBLE_S", 0.1):
                s = self.session(speed=4, button_device=fifo)
                s.input_devices = procfile
                s.start()
                st = self.waiting(s, "starting")
                self.assertEqual((st["button"], st["mouse"], st["ready_text"]), (True, False, session.READY_BUTTON))
                self.assertIn("panel: action " + session.READY_BUTTON, self.panel)
                self.assertIn("panel: keys " + session.KEYS_STEP_BUTTON, self.panel)
                press()
                self.waiting(s, "intro")
                press()
                self.waiting(s, "ready", "Fist.")
                press()                                       # Next
                self.wait_for(s, lambda st: st["state"] == "running", "the hold")
                press()                                       # pause
                self.wait_for(s, lambda st: st["state"] == "paused", "paused")
                with open(procfile, "a") as f:                # a mouse arrives
                    f.write(BT_MOUSE)
                press()                                       # resume
                st = self.waiting(s, "ready", "Open.")
                self.assertEqual((st["mouse"], st["ready_text"]), (True, session.READY_BUTTON_MOUSE))
                press()
                s.join(20)
            self.assertEqual(s.state, "done")
            names = [e["event"] for e in self.events(s)]
            self.assertEqual(names, ["take", "ready", "prompt", "pause", "resume", "wait", "ready", "prompt", "wait",
                                     "end"])
        finally:
            os.close(writer)

    def test_redo_and_stop(self):
        """Two presses redo the step, a hold stops the session; the hints say so."""
        fifo = os.path.join(self.tmp, "button")
        os.mkfifo(fifo)
        writer = os.open(fifo, os.O_RDWR)
        press = lambda: os.write(writer, PRESS + RELEASE)
        try:
            with mock.patch.multiple(session, BUTTON_DOUBLE_S=0.15, BUTTON_HOLD_S=0.4):
                s = self.session(speed=2, button_device=fifo)
                s.start()
                self.waiting(s, "starting")
                press()
                self.waiting(s, "intro")
                press()
                self.waiting(s, "ready", "Fist.")
                press()
                self.wait_for(s, lambda st: st["state"] == "running", "the hold")
                os.write(writer, taps(1, 1.1))                # twice: redo
                self.waiting(s, "ready", "Fist.")
                time.sleep(0.1)                               # (sooner would be a bounce of the last)
                press()
                self.wait_for(s, lambda st: st["state"] == "running", "the hold again")
                press()                                       # pause
                st = self.wait_for(s, lambda st: st["state"] == "paused", "paused")
                self.assertEqual(st["note"], session.RESUME_HINT_BUTTON)
                os.write(writer, PRESS)                       # held: stop
                s.join(20)
                os.write(writer, RELEASE)
            self.assertEqual(s.state, "stopped")
            names = [e["event"] for e in self.events(s)]
            self.assertEqual(names, ["take", "ready", "prompt", "redo", "wait", "ready", "prompt", "pause", "end"])
            self.assertIn("panel: keys " + session.KEYS_STEP_BUTTON, self.panel)
        finally:
            os.close(writer)

    def test_no_button(self):
        s = self.session(next_after=0.0, button=False, button_device="/nonexistent")
        s.start()
        s.join(20)
        self.assertEqual(s.state, "done")
        self.assertIn("panel: action " + session.READY_TEXT, self.panel)
        self.assertIn("panel: keys " + session.KEYS_STEP, self.panel)


class LightingTest(unittest.TestCase):
    """The measured lighting label: the mono cameras' ambient infrared."""

    def ring(self, slam, upper):
        return {"slam_left": {"mean": 70.0, "dark_mean": slam}, "slam_right": {"mean": 80.0, "dark_mean": slam},
                "upper_left": {"mean": 50.0, "dark_mean": upper}, "upper_right": {"mean": 20.0, "dark_mean": upper}}

    def test_indoor_and_daylight(self):
        self.assertEqual(session.classify_lighting(self.ring(3.0, 0.5)), "indoor")    # one lamp, 2026-10-02
        self.assertEqual(session.classify_lighting(self.ring(3.7, 0.7)), "indoor")    # a lamp-lit room
        self.assertEqual(session.classify_lighting(self.ring(20.0, 8.0)), "daylight")
        self.assertEqual(session.classify_lighting(self.ring(0.0, 0.0)), "")          # no dark frames yet
        self.assertEqual(session.classify_lighting(None), "")
        self.assertEqual(session.ambient_ir(self.ring(3.0, 0.5)), 1.75)

    def test_record(self):
        rec = session.lighting_record("auto", self.ring(3.0, 0.5))
        self.assertEqual((rec["chosen"], rec["source"], rec["measured"]), ("indoor", "measured", "indoor"))
        rec = session.lighting_record("dim", self.ring(3.0, 0.5))
        self.assertEqual((rec["chosen"], rec["source"], rec["measured"]), ("dim", "picked", "indoor"))
        rec = session.lighting_record("auto", None)
        self.assertEqual((rec["chosen"], rec["source"], rec["ring"]), ("", "measured", {}))


class ManyPartsTest(unittest.TestCase):
    """A take recorded in 40 parts (step mode stops the recording between steps): review reads
    them in order, export makes one stream, validate passes."""

    def setUp(self):
        if not takes.find_zstd():
            self.skipTest("no zstd")
        self.tmp = tempfile.mkdtemp(prefix="handrec-parts-test-")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_parts(self):
        make_session(self.tmp)
        tdir = os.path.join(self.tmp, "sessions", SESSION, "takes", "01-hand-size")
        t0 = 5 * 10 ** 12
        for part in range(1, 41):
            name = "sets.bin" if part == 1 else "sets-%d.bin" % part
            with open(os.path.join(tdir, name), "wb") as f:
                for i in range(3):   # 3 sets a part, 10 s apart between parts
                    f.write(fhset(t0 + part * 10 ** 10 + i * 10 ** 8, part))
        store = takes.Store(self.tmp)
        index = takes.take_index(tdir)
        self.assertEqual(len(index.parts), 40)
        self.assertEqual(len(index), 120)
        times = [index.time_ns(i) for i in range(len(index))]
        self.assertEqual(times, sorted(times))   # sets-10.bin after sets-9.bin
        self.assertAlmostEqual(index.duration_s(), 40 * 0.2)   # the gaps between parts don't count
        path = store.export(SESSION, low_priority=False)
        r = validate.validate(path)
        self.assertEqual(r.errors, [])
        self.assertEqual(r.summary["sets"], 120 + 30)


class StandaloneTest(unittest.TestCase):
    """The standalone Hand Recorder's tree (standalone.json at the top): ft-hands runs on the
    host, the version comes from the build info, and repairs point at its install command."""
    INFO = {"name": "frametop-hand-recorder", "version": "0.1.0", "frametop": "4ba49af",
            "reinstall": "run the install command again"}

    def test_build_info(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "standalone.json")
            self.assertIsNone(takes.standalone(path))
            with open(path, "w") as f:
                json.dump(self.INFO, f)
            self.assertEqual(takes.standalone(path), self.INFO)
            with open(path, "w") as f:
                f.write("[1]")
            self.assertIsNone(takes.standalone(path))
        with mock.patch.object(takes, "standalone", return_value=self.INFO):
            self.assertEqual(takes.tool_version(), "ft-handrec 4ba49af (frametop-hand-recorder 0.1.0)")

    def recorder_argv(self, standalone):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(session, "STANDALONE", standalone), \
                mock.patch.object(session, "in_container", return_value=False), \
                mock.patch.object(session.subprocess, "Popen") as popen:
            session.Recorder(d, 1, 10, None, None)
            return popen.call_args[0][0]

    def tracker_argv(self, standalone):
        fake = mock.Mock(ring=None)
        with mock.patch.object(session, "STANDALONE", standalone), \
                mock.patch.object(session.subprocess, "run") as run:
            session.Session._start_tracker(fake)
        return fake._start_unit.call_args[0][2], run

    def test_ft_hands_on_the_host(self):
        self.assertEqual(self.recorder_argv(self.INFO)[0], session.FT_HANDS)
        argv, run = self.tracker_argv(self.INFO)
        self.assertEqual(argv[0], session.FT_HANDS)
        self.assertIn("--no-gestures", argv)
        run.assert_not_called()   # no container to bring up

    def test_ft_hands_in_the_dev_container(self):
        self.assertTrue(self.recorder_argv(None)[0].endswith("distrobox"))
        argv, _ = self.tracker_argv(None)
        self.assertEqual(argv[:4], [os.path.expanduser("~/.local/bin/distrobox"), "enter", "dev", "--"])
        self.assertEqual(argv[4], session.FT_HANDS)

    def test_fix_hint(self):
        with mock.patch.object(session, "STANDALONE", self.INFO):
            self.assertEqual(session.fix_hint("hands/build.sh"), "run the install command again")
            text = session.camera_text({"status": "degraded", "reason": "2 of 4", "ring_missing": ["upper_left"]})
        with mock.patch.object(session, "STANDALONE", None):
            self.assertEqual(session.fix_hint("hands/build.sh"), "hands/build.sh")
        self.assertIn("run the install command again", text)
        self.assertNotIn("~/frametop", text)


if __name__ == "__main__":
    unittest.main()
