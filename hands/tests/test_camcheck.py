#!/usr/bin/env python3
"""Tests for hands/camcheck.py: the real XRService log of 2026-10-02 (a good start at 13:39,
the VCINT failure after the 17:02 wake) cut at several points, synthetic logs for the other
cases, and a made-up ft-camd ring. Nothing here touches SteamVR or the cameras.

  python3 hands/tests/test_camcheck.py
"""
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stdout

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import camcheck  # noqa: E402

REAL_LOG = os.path.expanduser("~/.local/share/Steam/logs/XRService-2026.10.02/XRService-13-39-32.log")


def L(t, text, level="INFO"):
    """A log line as XRService writes it (with its colour codes on INFO lines)."""
    if level == "INFO":
        return "Fri Oct 02 2026 %s.000000 INFO: \x1b[0;36m%s\x1b[0m" % (t, text)
    return "Fri Oct 02 2026 %s.000000 %s: %s" % (t, level, text)


START = [L("10:00:00", "XRService logging to /tmp/x.log. Use --showLogToConsole to see it here as well")]
GOOD_OPEN = [
    L("10:00:01", "FPGA state check: PASSTHRU (register value: 0x00011212)"),
    L("10:00:01", "Passthrough connected but FPGA is PASSTHRU - loading VCINT"),
    L("10:00:01", "Loading FPGA image: VCINT"),
    L("10:00:02", "FPGA image VCINT loaded and verified successfully"),
    L("10:00:02", "Upper cameras FPGA interleaving support: 1 (Driver features available = 1 | VCINT loaded = 1)"),
    L("10:00:02", "[buildMediaCtlSetupTasks] Created 6 tasks (4 tracking, 2 passthrough)"),
] + [L("10:00:03", "TrackingCameraInit: index: %d. video device: /dev/video%d. v4l subdevice: /dev/v4l-subdev3%d"
       % (i, n, i)) for i, n in enumerate((9, 13, 6, 7))]
CLOSE = [L("11:00:00", "[SystemdInhibitor] Received systemd suspend notification"),
         L("11:00:00", "[DeckardCaptureSource] Closing tracking camera interfaces camerasToUse: 1111"),
         L("11:00:01", "[DeckardCaptureSource] Streaming paused")]
RESUME = [L("11:30:00", "[SystemdInhibitor] Received systemd resume notification"),
          L("11:30:00", "Failed to read FPGA register 0x12 (exit code 256)", "ERROR"),
          L("11:30:00", "Passthrough connected but FPGA is ERROR/UNKNOWN - loading VCINT"),
          L("11:30:00", "Loading FPGA image: VCINT")]
FAIL = [L("11:30:01", "FPGA load failed (exit code 256): Loading VC Interleaving FPGA image", "ERROR"),
        "Loading FPGA image from bitstream file /usr/lib/deckard-fpga/images/csi_agg_deckard_ev1_1_vcint.binx",
        "  FAILED: FPGA config_done signal did not assert",
        L("11:30:01", "Failed to load VCINT FPGA image when passthrough cameras are connected", "ERROR"),
        L("11:30:01", "Upper cameras FPGA interleaving support: 0 (Driver features available = 1 | VCINT loaded = 0)"),
        L("11:30:01", "[buildMediaCtlSetupTasks] Created 4 tasks (2 tracking, 2 passthrough)"),
        L("11:30:01", "TrackingCameraInit: index: 0. video device: /dev/video9. v4l subdevice: /dev/v4l-subdev31"),
        L("11:30:01", "TrackingCameraInit: index: 1. video device: /dev/video13. v4l subdevice: /dev/v4l-subdev30"),
        L("11:30:02", "[DeckardCaptureSource] Streaming resumed (FPGA: PASSTHRU, VC interleaving: disabled)")]
# A wake that works prints no "Created N tasks" (2026-10-01 16:39), so an older one must not count.
RESUME_OK = [L("12:30:00", "[SystemdInhibitor] Received systemd resume notification"),
             L("12:30:00", "Passthrough connected but FPGA is ERROR/UNKNOWN - loading VCINT"),
             L("12:30:01", "FPGA image VCINT loaded and verified successfully"),
             L("12:30:01", "FPGA state check: VCINT (register value: 0x00021211)"),
             L("12:30:01", "Upper cameras FPGA interleaving support: 1 (Driver features available = 1 | VCINT loaded = 1)")] + \
            [L("12:30:01", "TrackingCameraInit: index: %d. video device: /dev/video%d. v4l subdevice: x" % (i, n))
             for i, n in enumerate((9, 13, 6, 7))] + \
            [L("12:30:02", "[DeckardCaptureSource] Streaming resumed (FPGA: VCINT, VC interleaving: enabled)")]
EXIT = [L("13:00:00", "XRService - main thread exiting"), L("13:00:00", "Exiting XRService")]
# The colour module unplugged while SteamVR runs (2026-10-05 12:42): XRService reopens the
# cameras with the side pair through the ISP on vfe0 and vfe1 (NV12); VCINT stays loaded, so the
# upper pair stays on vfe2.
UNPLUG = [L("12:42:25", "Received passthrough camera connection event (connected=0)"),
          L("12:42:25", "[DeckardCaptureSource] Closing tracking camera interfaces camerasToUse: 1111"),
          L("12:42:26", "FPGA state check: VCINT (register value: 0x00021211)"),
          L("12:42:26", "Upper cameras FPGA interleaving support: 1 (Driver features available = 1 | VCINT loaded = 1)"),
          L("12:42:26", "[buildMediaCtlSetupTasks] ISP enabled for tracking cameras (main VFE available)"),
          L("12:42:26", "[buildMediaCtlSetupTasks] Created 4 tasks (4 tracking, 0 passthrough)")] + \
         [L("12:42:26", "TrackingCameraInit: index: %d. video device: /dev/video%d. v4l subdevice: x" % (i, n))
          for i, n in enumerate((0, 3, 6, 7))]
# Started without the module (FrameEyeCameraFeed's layout): the upper pair on vfe3 and vfe4.
NO_MODULE = [L("10:00:02", "[buildMediaCtlSetupTasks] ISP enabled for tracking cameras (main VFE available)"),
             L("10:00:02", "[buildMediaCtlSetupTasks] Created 4 tasks (4 tracking, 0 passthrough)")] + \
            [L("10:00:03", "TrackingCameraInit: index: %d. video device: /dev/video%d. v4l subdevice: x" % (i, n))
             for i, n in enumerate((0, 3, 9, 13))]


def state(lines):
    return camcheck.LogState("test").feed_text("\n".join(lines))


class SyntheticLogs(unittest.TestCase):
    def test_good_start(self):
        st = state(START + GOOD_OPEN)
        self.assertEqual(st.verdict()[:2], ("ok", "4 tracking cameras running"))
        self.assertEqual(st.episode["vcint"], "ok")
        self.assertEqual(st.upper_nodes(), (6, 7))

    def test_good_start_tasks_only(self):
        # "Created 6 tasks (4 tracking ...)" and no camera init yet: 4 tracking cameras planned
        self.assertEqual(state(START + GOOD_OPEN[:6]).verdict()[0], "ok")

    def test_starting(self):
        self.assertEqual(state(START + GOOD_OPEN[:3]).verdict()[:2], ("unknown", "the cameras are starting"))

    def test_vcint_already_loaded(self):
        # A SteamVR restart in the same boot (2026-10-01 15:27): the FPGA still has VCINT.
        lines = START + [L("10:00:01", "FPGA state check: VCINT (register value: 0x00021211)"),
                         L("10:00:01", "Upper cameras FPGA interleaving support: 1 (Driver features available = 1 | VCINT loaded = 1)"),
                         L("10:00:01", "[buildMediaCtlSetupTasks] Created 6 tasks (4 tracking, 2 passthrough)")]
        st = state(lines)
        self.assertEqual(st.verdict()[0], "ok")
        self.assertEqual(st.episode["vcint"], "loaded")

    def test_asleep(self):
        status, reason, ev = state(START + GOOD_OPEN + CLOSE).verdict()
        self.assertEqual(status, "unknown")
        self.assertIn("closed", reason)
        self.assertTrue(any("Closing tracking camera interfaces" in e for e in ev))

    def test_vcint_failure(self):
        st = state(START + GOOD_OPEN + CLOSE + RESUME + FAIL)
        status, reason, ev = st.verdict()
        self.assertEqual((status, reason), ("degraded", camcheck.VCINT_REASON))
        self.assertTrue(any("Created 4 tasks (2 tracking, 2 passthrough)" in e for e in ev))
        self.assertTrue(any("Closing tracking camera interfaces" in e for e in ev))
        self.assertFalse(any("\x1b" in e for e in ev))
        self.assertEqual(st.snapshot()["failure"], "test@11:30:01")
        self.assertEqual(len(st.failures), 1)

    def test_failure_seen_before_the_cameras_start(self):
        # the failure line alone (camera init not logged yet): degraded already
        st = state(START + GOOD_OPEN + CLOSE + RESUME + FAIL[:4])
        self.assertEqual(st.verdict()[0], "degraded")

    def test_wake_after_failure_works(self):
        st = state(START + GOOD_OPEN + CLOSE + RESUME + FAIL + CLOSE + RESUME_OK)
        self.assertEqual(st.verdict()[0], "ok")
        self.assertEqual(st.snapshot()["failure"], "")
        self.assertEqual(len(st.failures), 1)   # still remembered for the log's history

    def test_fewer_cameras_without_vcint(self):
        lines = START + [L("10:00:01", "[buildMediaCtlSetupTasks] Created 4 tasks (2 tracking, 2 passthrough)")]
        status, reason, _ = state(lines).verdict()
        self.assertEqual(status, "degraded")
        self.assertEqual(reason, "only 2 of 4 tracking cameras running")

    def test_camera_map_with_module(self):
        st = state(START + GOOD_OPEN)
        self.assertEqual(st.camera_map(), {"slam_left": 9, "slam_right": 13, "upper_left": 6, "upper_right": 7})

    def test_camera_map_by_found_camera(self):
        """XRService's own naming (2026-10-05's log): index 0 is slam_right, by its subdev."""
        found = [L("10:00:00", "DeckardCaptureSource: Found camera '%s': interface=msm_csiphy%d v4l_subdev=/dev/v4l-subdev%d "
                   "driver=/sys/bus/i2c/drivers/%s" % f) for f in (
            ("slam_left", 0, 30, "og01a1b/4-0060"), ("slam_right", 1, 31, "og01a1b/4-0036"),
            ("upper_left", 0, 32, "og0ve10/5-0060"), ("upper_right", 1, 33, "og0ve10/5-003e"))]
        inits = [L("10:00:03", "TrackingCameraInit: index: %d. video device: /dev/video%d. v4l subdevice: /dev/v4l-subdev%d"
                   % f) for f in ((0, 9, 31), (1, 13, 30), (2, 6, 32), (3, 7, 33))]
        st = state(START + found + GOOD_OPEN[:-4] + inits)
        self.assertEqual(st.camera_map(), {"slam_right": 9, "slam_left": 13, "upper_left": 6, "upper_right": 7})
        # without the colour module the sides are on video0 and video3, named the same way
        unplug = [L("10:30:00", "TrackingCameraInit: index: %d. video device: /dev/video%d. v4l subdevice: /dev/v4l-subdev%d"
                    % f) for f in ((0, 0, 31), (1, 3, 30))]
        st = state(START + found + GOOD_OPEN[:-4] + inits + CLOSE + unplug)
        self.assertEqual(st.camera_map(), {"slam_right": 0, "slam_left": 3})
        # a new instance forgets the old one's names: back to the index order
        self.assertEqual(state(START + found + START + GOOD_OPEN).camera_map()["slam_left"], 9)

    def test_module_unplugged(self):
        st = state(START + GOOD_OPEN + UNPLUG)
        self.assertEqual(st.verdict()[0], "ok")
        self.assertIs(st.episode["isp"], True)
        self.assertEqual(st.camera_map(), {"slam_left": 0, "slam_right": 3, "upper_left": 6, "upper_right": 7})
        self.assertEqual(st.tracking_nodes(), (0, 3, 6, 7))

    def test_started_without_module(self):
        st = state(START + NO_MODULE)
        self.assertEqual(st.verdict()[0], "ok")
        self.assertEqual(st.camera_map(), {"slam_left": 0, "slam_right": 3, "upper_left": 9, "upper_right": 13})
        self.assertEqual(st.upper_nodes(), (9, 13))

    def test_exited_and_empty(self):
        self.assertEqual(state(START + GOOD_OPEN + EXIT).verdict()[0], "unknown")
        self.assertEqual(state([]).verdict()[0], "unknown")
        self.assertEqual(state(START).verdict()[:2], ("unknown", "the cameras haven't started yet in this log"))

    def test_new_instance_resets(self):
        st = state(START + GOOD_OPEN + CLOSE + RESUME + FAIL + START + GOOD_OPEN)
        self.assertEqual(st.verdict()[0], "ok")
        self.assertEqual(st.failures, [])

    def test_incremental_equals_whole(self):
        text = "\n".join(START + GOOD_OPEN + CLOSE + RESUME + FAIL)
        st = camcheck.LogState("test")
        for line in text.split("\n"):
            st.feed(line + "\n")
        self.assertEqual(st.snapshot(), state(START + GOOD_OPEN + CLOSE + RESUME + FAIL).snapshot())


@unittest.skipUnless(os.path.exists(REAL_LOG), "the 2026-10-02 XRService log isn't on this machine")
class RealLog(unittest.TestCase):
    """The log with both the good 13:39 start and the 17:02 failure, cut in copies in /tmp."""

    @classmethod
    def setUpClass(cls):
        with open(REAL_LOG, errors="replace") as f:
            cls.lines = f.read().split("\n")
        cls.tmp = tempfile.mkdtemp(prefix="camcheck-test-")

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def cut_after(self, needle, nth=1):
        """The log up to and including the nth line containing needle, as a file in /tmp."""
        seen = 0
        for i, line in enumerate(self.lines):
            if needle in line:
                seen += 1
                if seen == nth:
                    path = os.path.join(self.tmp, "cut-%d.log" % i)
                    with open(path, "w") as f:
                        f.write("\n".join(self.lines[:i + 1]) + "\n")
                    return path
        self.fail("no line %d with %r" % (nth, needle))

    def check(self, path):
        return camcheck.check(log=path, proc=False, ring=False)

    def test_cut_points(self):
        cases = [
            ("Loading FPGA image: VCINT", 1, "unknown", "the cameras are starting"),
            ("FPGA image VCINT loaded and verified successfully", 1, "unknown", "the cameras are starting"),
            ("Created 6 tasks (4 tracking, 2 passthrough)", 1, "ok", None),
            ("TrackingCameraInit: index: 3.", 1, "ok", None),
            ("Upper cameras are not in sync", 1, "ok", None),         # 15:45: a resync, not a failure
            ("Closing tracking camera interfaces", 1, "unknown", None),   # 16:41: asleep
            ("Passthrough connected but FPGA is ERROR/UNKNOWN", 1, "unknown", "the cameras are starting"),
            ("Failed to load VCINT FPGA image", 1, "degraded", camcheck.VCINT_REASON),
            ("Created 4 tasks (2 tracking, 2 passthrough)", 1, "degraded", camcheck.VCINT_REASON),
            ("Streaming resumed (FPGA: PASSTHRU", 1, "degraded", camcheck.VCINT_REASON),
        ]
        for needle, nth, status, reason in cases:
            with self.subTest(cut=needle):
                r = self.check(self.cut_after(needle, nth))
                self.assertEqual(r["status"], status, r["summary"])
                if reason:
                    self.assertEqual(r["reason"], reason)
        r = self.check(self.cut_after("Closing tracking camera interfaces"))
        self.assertIn("closed", r["reason"])

    def test_whole_log(self):
        r = self.check(REAL_LOG)
        self.assertEqual(r["summary"], camcheck.DEGRADED_VCINT)
        ev = "\n".join(r["evidence"])
        for want in ("17:02:47 Failed to load VCINT FPGA image", "interleaving support: 0",
                     "Created 4 tasks (2 tracking, 2 passthrough)", "/dev/video9", "/dev/video13",
                     "Closing tracking camera interfaces"):
            self.assertIn(want, ev)
        self.assertNotIn("/dev/video6", ev)   # the 13:39 start isn't this episode's evidence
        self.assertTrue(r["failure"].endswith("@17:02:47"))
        self.assertTrue(camcheck.is_vcint_failure(r))

    def test_cli(self):
        out = io.StringIO()
        with redirect_stdout(out):
            code = camcheck.main(["--log", REAL_LOG, "--no-proc", "--no-ring", "--json"])
        self.assertEqual(code, 1)
        self.assertEqual(json.loads(out.getvalue())["status"], "degraded")
        out = io.StringIO()
        with redirect_stdout(out):
            code = camcheck.main(["--log", self.cut_after("TrackingCameraInit: index: 3."), "--no-proc", "--no-ring"])
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue().split("\n")[0], "ok")


def make_ring(path, mono_names, alive=True, nodes=None):
    """A ring header as ft-camd writes it (camd/fhring.h), no frames."""
    cams = [(b"og01a1b", n.encode(), nodes[i] if nodes else 9 + i) for i, n in enumerate(mono_names)]
    hb = time.clock_gettime_ns(time.CLOCK_MONOTONIC) if alive else 1
    data = bytearray(camcheck.RING_HDR.pack(b"FHRING01", 1, 0, len(cams), 0, 0, 4242, 0))
    struct.pack_into("<Q", data, 40, hb)
    for sensor, name, node in cams:
        data += camcheck.RING_CAM.pack(sensor, name, node, 0, 1056, 1024, 1056, 16, 0, 0, 0, 0, 0, 0, 0.0)
    with open(path, "wb") as f:
        f.write(bytes(data))


class Ring(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="camcheck-ring-")
        self.ring = os.path.join(self.tmp, "cam-ring")
        self.log = os.path.join(self.tmp, "x.log")
        with open(self.log, "w") as f:
            f.write("\n".join(START + GOOD_OPEN) + "\n")

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def test_two_cameras_in_ring(self):
        make_ring(self.ring, ["slam_video9", "slam_video13"])
        r = camcheck.check(log=self.log, proc=False, ring_path=self.ring)
        self.assertEqual(r["status"], "degraded")
        self.assertIn("ft-camd publishes only 2 of 4", r["reason"])

    def test_ring_missing_the_side_cameras(self):
        # an ft-camd from before NV12 support, without the colour module: only the upper pair
        with open(self.log, "w") as f:
            f.write("\n".join(START + NO_MODULE) + "\n")
        make_ring(self.ring, ["og0ve10_5-003e_video9", "og0ve10_5-0060_video13"], nodes=[9, 13])
        r = camcheck.check(log=self.log, proc=False, ring_path=self.ring)
        self.assertEqual(r["status"], "degraded")
        self.assertEqual(r["ring_missing"], [0, 3])
        self.assertIn("missing video0 video3", r["reason"])
        self.assertTrue(camcheck.is_ring_short(r))
        self.assertFalse(camcheck.is_vcint_failure(r))
        self.assertEqual(r["map"]["slam_left"]["node"], 0)

    def test_four_cameras_in_ring(self):
        make_ring(self.ring, ["a_video9", "b_video13", "c_video6", "d_video7", "a_video9_dk"])
        r = camcheck.check(log=self.log, proc=False, ring_path=self.ring)
        self.assertEqual(r["status"], "ok")
        self.assertEqual(len(r["ring"]["mono"]), 4)   # the dark twin doesn't count

    def test_stale_ring_ignored(self):
        make_ring(self.ring, ["slam_video9"], alive=False)
        r = camcheck.check(log=self.log, proc=False, ring_path=self.ring)
        self.assertEqual(r["status"], "ok")
        self.assertIn("stale ring", "\n".join(r["evidence"]))

    def test_no_ring(self):
        r = camcheck.check(log=self.log, proc=False, ring_path=os.path.join(self.tmp, "none"))
        self.assertEqual(r["status"], "ok")
        self.assertIsNone(r["ring"])


if __name__ == "__main__":
    unittest.main()
