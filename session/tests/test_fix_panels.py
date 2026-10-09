#!/usr/bin/env python3
"""Tests for session/fix-panels.py: issue #18's config (the taskbar saved on spare output 8
of a 3-screen desktop), panels that are fine, an edge that's taken, and a real run of
kwriteconfig6 on a copy. Nothing here touches the running desktop or its config.

  python3 session/tests/test_fix_panels.py
"""
import importlib.util
import os
import resource
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.realpath(__file__))
SCRIPT = os.path.join(HERE, "..", "fix-panels.py")
spec = importlib.util.spec_from_file_location("fix_panels", SCRIPT)
fp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fp)


def desktop(cid, screen):
    return f"""[Containments][{cid}]
activityId=7e1d1a3c-0000-4000-8000-000000000000
formfactor=0
immutability=1
lastScreen={screen}
location=0
plugin=org.kde.plasma.folder
wallpaperplugin=org.kde.image
"""


def panel(cid, screen, location=4, tray=None):
    text = f"""[Containments][{cid}]
activityId=
formfactor=2
immutability=1
lastScreen={screen}
location={location}
plugin=org.kde.panel
wallpaperplugin=org.kde.image

[Containments][{cid}][Applets][{cid}1]
immutability=1
plugin=org.kde.plasma.kickoff
"""
    if tray:
        text += f"""
[Containments][{cid}][Applets][{cid}2]
immutability=1
plugin=org.kde.plasma.systemtray

[Containments][{cid}][Applets][{cid}2][Configuration]
PreloadWeight=90
SystrayContainmentId={tray}

[Containments][{tray}]
activityId=
formfactor=2
immutability=1
lastScreen={screen}
location={location}
plugin=org.kde.plasma.private.systemtray
popupHeight=432
"""
    return text


ISSUE_18 = "\n".join([desktop(1, 0), desktop(18, 1), desktop(19, 2)] + [desktop(20 + i, 3 + i) for i in range(8)]
                     + [panel(10, 8, tray=11), "[ScreenMapping]\nitemsOnDisabledScreens=\n"])


class Plan(unittest.TestCase):
    def test_issue_18(self):
        moves, kept = fp.plan(fp.parse(ISSUE_18), 3)
        self.assertEqual(moves, [("10", 8, ["10", "11"])])
        self.assertEqual(kept, [])

    def test_panel_on_a_screen_stays(self):
        for screen in (0, 2, -1):
            moves, kept = fp.plan(fp.parse(desktop(1, 0) + panel(2, screen, tray=8)), 3)
            self.assertEqual((moves, kept), ([], []), screen)

    def test_tray_is_not_a_panel(self):
        found = fp.panels(fp.parse(panel(2, 0, tray=8)))
        self.assertEqual(list(found), ["2"])
        self.assertEqual(found["2"]["trays"], ["8"])

    def test_desktops_never_move(self):
        moves, _ = fp.plan(fp.parse(ISSUE_18), 1)
        self.assertEqual([m[0] for m in moves], ["10"])

    def test_edge_taken_on_first_screen(self):
        text = panel(2, 0, tray=8) + panel(10, 8, tray=11)
        moves, kept = fp.plan(fp.parse(text), 3)
        self.assertEqual(moves, [])
        self.assertEqual([k[:2] for k in kept], [("10", 8)])

    def test_other_edge_is_free(self):
        moves, kept = fp.plan(fp.parse(panel(2, 0) + panel(10, 4, location=3)), 3)
        self.assertEqual(moves, [("10", 4, ["10"])])
        self.assertEqual(kept, [])

    def test_two_lost_on_one_edge(self):
        moves, kept = fp.plan(fp.parse(panel(10, 5) + panel(12, 8)), 3)
        self.assertEqual([m[0] for m in moves], ["10"])
        self.assertEqual([k[0] for k in kept], ["12"])

    def test_screen_count_went_down(self):
        moves, _ = fp.plan(fp.parse(panel(2, 1, location=3)), 1)
        self.assertEqual(moves, [("2", 1, ["2"])])


class Run(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.dir.name, "plasma-org.kde.plasma.desktop-appletsrc")
        with open(self.path, "w") as f:
            f.write(ISSUE_18)

    def tearDown(self):
        self.dir.cleanup()

    def run_script(self, *args):
        return subprocess.run([sys.executable, SCRIPT, "--file", self.path, "--screens", "3", *args],
                              capture_output=True, text=True)

    def test_check_reports_and_changes_nothing(self):
        r = self.run_script("--check")
        self.assertEqual(r.returncode, 1)
        self.assertIn("panel 10: screen 8, bottom (lost", r.stdout)
        with open(self.path) as f:
            self.assertEqual(f.read(), ISSUE_18)

    def test_repair(self):
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("moved it to the first screen", r.stderr)
        with open(self.path) as f:
            groups = fp.parse(f.read())
        self.assertEqual(groups[("Containments", "10")]["lastScreen"], "0")
        self.assertEqual(groups[("Containments", "11")]["lastScreen"], "0")
        self.assertEqual(groups[("Containments", "10", "Applets", "101")]["plugin"], "org.kde.plasma.kickoff")
        self.assertEqual(groups[("Containments", "25")]["lastScreen"], "8")  # a spare's desktop
        for bak in (".ft-bak", ".ft-bak.last"):
            with open(self.path + bak) as f:
                self.assertEqual(f.read(), ISSUE_18, bak)
        # A second run finds nothing to do and makes no backup.
        os.remove(self.path + ".ft-bak")
        os.remove(self.path + ".ft-bak.last")
        r = self.run_script()
        self.assertEqual((r.returncode, r.stderr), (0, ""))
        self.assertEqual(os.listdir(self.dir.name), [os.path.basename(self.path)])
        self.assertEqual(self.run_script("--check").returncode, 0)

    def test_no_config_yet(self):
        os.remove(self.path)
        self.assertEqual(self.run_script().returncode, 0)
        self.assertEqual(self.run_script("--check").returncode, 0)

    def test_backup_is_written_once(self):
        """A backup that's already there, the config before an earlier repair, stays as it
        was, and the repair still goes ahead."""
        with open(self.path + ".ft-bak", "w") as f:
            f.write("the config before an earlier repair\n")
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.path + ".ft-bak") as f:
            self.assertEqual(f.read(), "the config before an earlier repair\n")
        with open(self.path) as f:
            groups = fp.parse(f.read())
        self.assertEqual(groups[("Containments", "10")]["lastScreen"], "0")
        self.assertEqual(groups[("Containments", "11")]["lastScreen"], "0")

    def test_last_backup_is_from_before_each_repair(self):
        """<file>.ft-bak.last is the config as it was just before the latest repair, so that
        repair can be undone without losing what changed since the first; <file>.ft-bak stays
        the config before the first repair. The log names both."""
        bak = self.path + ".ft-bak"
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        # The panel is lost again later, after a change of the user's own (another widget).
        changed = ISSUE_18.replace("org.kde.plasma.kickoff", "org.kde.plasma.trash")
        with open(self.path, "w") as f:
            f.write(changed)
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(bak + ".last") as f:
            self.assertEqual(f.read(), changed)
        with open(bak) as f:
            self.assertEqual(f.read(), ISSUE_18)
        with open(self.path) as f:
            self.assertEqual(fp.parse(f.read())[("Containments", "10")]["lastScreen"], "0")
        self.assertIn(bak + ".last ", r.stderr)
        self.assertIn(bak + " ", r.stderr)

    def test_failed_backup_leaves_nothing(self):
        """A backup cut off partway (a full disk; here a 1 KiB limit on file size) leaves no
        backup and no temporary file behind, and the panels aren't touched without one. The
        next start, with room, makes the backup and repairs."""
        self.assertGreater(len(ISSUE_18), 1024)

        def small_files():
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))
        r = subprocess.run([sys.executable, SCRIPT, "--file", self.path, "--screens", "3"],
                           capture_output=True, text=True, preexec_fn=small_files)
        self.assertEqual(os.listdir(self.dir.name), [os.path.basename(self.path)])
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("couldn't back up", r.stderr)
        with open(self.path) as f:
            self.assertEqual(f.read(), ISSUE_18)
        r = self.run_script()
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.path + ".ft-bak") as f:
            self.assertEqual(f.read(), ISSUE_18)
        with open(self.path) as f:
            self.assertEqual(fp.parse(f.read())[("Containments", "10")]["lastScreen"], "0")


if __name__ == "__main__":
    unittest.main()
