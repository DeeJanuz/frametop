#!/usr/bin/env python3
"""Offline test of picking a release for a SteamOS build: get.sh's release_pick, on lists
pack/release-manifest.py writes. No network, podman, or install: it runs release_pick alone
(get.sh without its last line, which would run the installer) on lists in a temp folder.

  pack/test/release-test.py
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "..")
TMP = tempfile.mkdtemp(prefix="ft-release-test-")
with open(os.path.join(REPO, "get.sh")) as f:
    GET = f.read().rsplit('\nmain "$@"', 1)[0]  # the functions, without running main

OURS = "20260922.6101926"   # tested
NEW = "20261007.6180005"    # in no list
BAD = "20261001.6150000"    # breaks 0.3.0, fixed in 0.3.1
IMG = "ghcr.io/deejanuz/frametop@sha256:" + "a" * 64
IMG2 = "ghcr.io/deejanuz/frametop@sha256:" + "b" * 64

failures = []


def check(label, got, want):
    ok = got == want
    print(("ok    " if ok else "FAIL  ") + label + ("" if ok else f": got {got!r}, want {want!r}"), flush=True)
    if not ok:
        failures.append(label)


def write(name, obj):
    path = os.path.join(TMP, name)
    with open(path, "w") as f:
        f.write(obj if isinstance(obj, str) else json.dumps(obj))
    return path


def manifest(*args, steamos=None):
    cmd = [sys.executable, os.path.join(REPO, "pack", "release-manifest.py"), *args]
    if steamos is not None:
        cmd += ["--steamos", write("steamos.json", steamos)]
    return subprocess.run(cmd, capture_output=True, text=True)


def pick(path, build, want=""):
    r = subprocess.run(["bash", "-c", GET + '\nrelease_pick "$@"', "get.sh", path, build, want],
                       capture_output=True, text=True)
    return r.returncode, r.stdout.splitlines()


steamos = {"tested": [{"build": OURS, "version": "0.3.0", "steamvr": "2.17.10"}],
           "broken": [{"build": BAD, "version": "0.4.0", "reason": "the 3D mouse\nhas no laser",
                       "fixed_in": "0.3.1"}]}

r = manifest("--channel", "experimental", "--version", "0.3.0-exp.1234567", "--commit", "1234567",
             "--image", IMG, steamos=steamos)
check("the generator writes a list", r.returncode, 0)
old = write("old.json", r.stdout)
r = manifest("--channel", "experimental", "--version", "0.3.1-exp.89abcde", "--commit", "89abcde",
             "--image", IMG2, "--previous", old, steamos=steamos)
both = write("both.json", r.stdout)
listed = json.loads(r.stdout)
check("the new release goes first, the old one after",
      [x["version"] for x in listed["releases"]], ["0.3.1-exp.89abcde", "0.3.0-exp.1234567"])
check("a broken build stays listed for releases before its fix",
      [len(x["steamos"]["broken"]) for x in listed["releases"]], [0, 1])
r = manifest("--channel", "experimental", "--version", "0.3.1", "--commit", "89abcde",
             "--image", IMG2 + "x", steamos=steamos)
check("the generator refuses a malformed image", r.returncode, 2)

code, out = pick(both, OURS)
check("a tested build: the newest release, tested", (code, out[:3]), (0, ["tested", "0.3.1-exp.89abcde", IMG2]))
check("and the list's channel", out[4], "experimental")
code, out = pick(both, NEW)
check("a build in no list: the newest release, untested", (code, out[:2]), (0, ["untested", "0.3.1-exp.89abcde"]))
check("which says where it was tested", out[5], "tested on SteamOS 0.3.0 (build 20260922.6101926)")

only_old = write("only-old.json", Path(old).read_text())
code, out = pick(only_old, BAD)
check("a build that breaks the only release: broken", (code, out[0]), (0, "broken"))
check("with the reason on one line, and the fix", out[5], "the 3D mouse has no laser (fixed in Frametop 0.3.1)")

# A new release that breaks on a build: the one before still fits it (the pin, the other way).
steamos2 = {"tested": steamos["tested"], "broken": [{"build": NEW, "reason": "no gaze", "fixed_in": "0.4.0"}]}
r = manifest("--channel", "stable", "--previous", both, steamos=steamos2)
refreshed = write("refreshed.json", r.stdout)
check("--previous alone refreshes the tables", [len(x["steamos"]["broken"]) for x in json.loads(r.stdout)["releases"]],
      [1, 1])
code, out = pick(refreshed, NEW)
check("a build every release breaks on: broken", out[0], "broken")
steamos3 = {"tested": steamos["tested"], "broken": [{"build": NEW, "reason": "no gaze", "fixed_in": "0.3.1"}]}
code, out = pick(write("pinned.json", manifest("--channel", "stable", "--previous", both, steamos=steamos3).stdout), NEW)
check("a build only the old release breaks on: the new one, untested", out[:2], ["untested", "0.3.1-exp.89abcde"])

# The newest release needs something an older SteamOS build lacks: that build keeps getting
# the release before, which was tested on it.
steamos4 = {"tested": steamos["tested"], "broken": [{"build": OURS, "reason": "needs SteamVR 2.18", "from": "0.3.1"}]}
code, out = pick(write("from.json", manifest("--channel", "stable", "--previous", both, steamos=steamos4).stdout), OURS)
check("a build the new release breaks on gets the one before, tested", out[:2], ["tested", "0.3.0-exp.1234567"])

code, out = pick(both, OURS, "0.3.0-exp.1234567")
check("--version picks that release", out[:2], ["tested", "0.3.0-exp.1234567"])
code, out = pick(both, OURS, "9.9.9")
check("--version of a release that isn't listed fails", (code, out), (1, []))

for label, bad in (("an image without a digest", {"version": "1.0", "commit": "1234567", "image": "ghcr.io/x/frametop:latest"}),
                   ("a version with a slash", {"version": "../1.0", "commit": "1234567", "image": IMG}),
                   ("a commit that isn't hex", {"version": "1.0", "commit": "$(reboot)", "image": IMG})):
    code, out = pick(write("bad.json", {"schema": "frametop.release/v1", "releases": [bad]}), OURS)
    check(f"a list with {label} fails", (code, out), (1, []))
code, out = pick(write("notjson.json", "<html>"), OURS)
check("a page that isn't JSON fails", (code, out), (1, []))
code, out = pick(write("other.json", {"schema": "other", "releases": []}), OURS)
check("another schema fails", (code, out), (1, []))
code, out = pick(write("oddtable.json", {"schema": "frametop.release/v1", "releases": [
    {"version": "1.0", "commit": "1234567", "image": IMG, "steamos": ["not", "a", "table"]}]}), OURS)
check("an odd SteamOS table counts as untested", (code, out[:2]), (0, ["untested", "1.0"]))

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
sys.exit(1 if failures else 0)
