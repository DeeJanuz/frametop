#!/usr/bin/env python3
"""Offline test of a release's SteamOS check and its frametop-release.json: install-release.sh's
check_release (the script without its last line, which would install), on files
pack/release-info.py writes from a small made-up OCI archive. No network, podman, or install.

  pack/test/release-test.py
"""
import hashlib
import io
import json
import os
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "..")
TMP = tempfile.mkdtemp(prefix="ft-release-test-")
SCRIPT = Path(REPO, "pack", "install-release.sh").read_text().rsplit('\nmain "$@"', 1)[0]

OURS = "20260922.6101926"   # tested
NEW = "20261007.6180005"    # in no table
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


def oci_archive(path, config=b'{"architecture":"arm64"}'):
    """An OCI archive with one image: index.json -> manifest -> config."""
    def digest(b):
        return hashlib.sha256(b).hexdigest()
    manifest = json.dumps({"schemaVersion": 2, "config": {"digest": "sha256:" + digest(config)}, "layers": []}).encode()
    index = json.dumps({"schemaVersion": 2, "manifests": [{"digest": "sha256:" + digest(manifest)}]}).encode()
    with tarfile.open(path, "w") as tar:
        for name, data in (("index.json", index), (f"blobs/sha256/{digest(manifest)}", manifest),
                           (f"blobs/sha256/{digest(config)}", config)):
            info = tarfile.TarInfo(name)
            info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return digest(config)


def info(version, steamos, *extra):
    table = write("steamos.json", steamos)
    r = subprocess.run([sys.executable, os.path.join(REPO, "pack", "release-info.py"), "--image-file", IMAGE,
                        "--version", version, "--commit", "1234567", "--steamos", table, *extra],
                       capture_output=True, text=True)
    return r.returncode, r.stdout


def pick(release, build, live=""):
    r = subprocess.run(["bash", "-c", SCRIPT + '\ncheck_release "$@"', "install-release.sh", release, live, build],
                       capture_output=True, text=True)
    return r.returncode, r.stdout.splitlines()


IMAGE = os.path.join(TMP, "frametop-image.tar")
ID = oci_archive(IMAGE)
SHA = hashlib.sha256(Path(IMAGE).read_bytes()).hexdigest()
tested = [{"build": OURS, "version": "0.3.0", "steamvr": "2.17.10"}]

code, out = info("0.3.0", {"tested": tested, "broken": []})
check("release-info.py writes it", code, 0)
rel = json.loads(out)
check("with the image file's sha256 and the image's ID", (rel["image"]["sha256"], rel["image"]["id"]), (SHA, ID))
check("a version without a \"-\" is stable", rel["channel"], "stable")
check("an experimental one", json.loads(info("0.3.0-exp.1", {"tested": tested})[1])["channel"], "experimental")
check("release-info.py refuses a malformed version", info("../0.3", {"tested": tested})[0], 2)
two = os.path.join(TMP, "two.tar")
with tarfile.open(two, "w") as tar:
    data = json.dumps({"manifests": [{"digest": "sha256:a"}, {"digest": "sha256:b"}]}).encode()
    t = tarfile.TarInfo("index.json")
    t.size = len(data)
    tar.addfile(t, io.BytesIO(data))
r = subprocess.run([sys.executable, os.path.join(REPO, "pack", "release-info.py"), "--image-file", two,
                    "--version", "1.0", "--commit", "1234567"], capture_output=True, text=True)
check("and an archive with two images", r.returncode != 0, True)

stable = write("stable.json", out)
code, out = pick(stable, OURS)
check("a tested build: tested", (code, out[0], out[1]), (0, "tested", "0.3.0"))
check("and the image's sha256 and ID come through", out[4:6], [SHA, ID])
code, out = pick(stable, NEW)
check("a build in no table: untested", out[0], "untested")
check("which says where it was tested", out[6], "tested on SteamOS 0.3.0 (build 20260922.6101926)")

broken = {"tested": tested, "broken": [{"build": NEW, "reason": "the 3D mouse\nhas no laser", "fixed_in": "0.3.1"}]}
code, out = pick(write("b.json", info("0.3.0", broken)[1]), NEW)
check("a build broken for this release: broken", out[0], "broken")
check("with the reason on one line, and the fix", out[6], "the 3D mouse has no laser (fixed in Frametop 0.3.1)")
code, out = pick(write("b2.json", info("0.3.1-exp.2", broken)[1]), NEW)
check("the release with the fix isn't broken there", out[0], "untested")

needs = {"tested": tested, "broken": [{"build": OURS, "reason": "needs SteamVR 2.18", "from": "0.4.0"}]}
check("a break from a later release doesn't count for this one", pick(write("n1.json", info("0.3.0", needs)[1]), OURS)[1][0],
      "tested")
check("it counts from that release on, over tested", pick(write("n2.json", info("0.4.0", needs)[1]), OURS)[1][0], "broken")

live = write("live.json", {"tested": tested + [{"build": NEW, "version": "0.5.5"}], "broken": []})
check("the newest table from GitHub counts over the release's", pick(stable, NEW, live)[1][0], "tested")
check("a table that isn't one is ignored", pick(stable, NEW, write("junk.json", "<html>"))[1][0], "untested")
live_broken = write("live-broken.json", {"tested": tested, "broken": [{"build": OURS, "reason": "x", "fixed_in": "0.3.1"}]})
check("and it can say a build broke after the release came out", pick(stable, OURS, live_broken)[1][0], "broken")

good = json.loads(Path(stable).read_text())
for label, change in (("a version with a slash", {"version": "../1.0"}),
                      ("a commit that isn't hex", {"commit": "$(reboot)"}),
                      ("an image ID that isn't one", {"image": {"sha256": SHA, "id": "latest"}}),
                      ("no image sha256", {"image": {"id": ID}}),
                      ("another schema", {"schema": "frametop.release/v0"})):
    check(f"a release file with {label} fails", pick(write("bad.json", {**good, **change}), OURS), (1, []))
check("a release file that isn't JSON fails", pick(write("notjson.json", "{"), OURS), (1, []))

print("FAILED: " + ", ".join(failures) if failures else "all passed", flush=True)
sys.exit(1 if failures else 0)
