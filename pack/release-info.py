#!/usr/bin/env python3
"""Write frametop-release.json, for a release's Frametop.zip (framedrop/build.sh --image):
the version, the commit, the channel, the image file's sha256, the image's ID (its config's
digest: podman's ID for it once loaded), and the SteamOS table, pack/steamos.json.
pack/install-release.sh reads it.

  pack/release-info.py --image-file frametop-image.tar --version 0.3.0 --commit SHA \\
      [--channel stable|experimental] > frametop-release.json

The image file is an OCI archive (podman save --format oci-archive). The channel defaults to
experimental for a version with a "-" (0.3.0-exp.1), else stable.
"""
import argparse
import hashlib
import json
import re
import sys
import tarfile
from pathlib import Path

HERE = Path(__file__).resolve().parent


def image_id(path):
    """The config digest of the one image in an OCI archive."""
    with tarfile.open(path) as tar:
        def blob(digest):
            algo, _, hexd = digest.partition(":")
            return json.load(tar.extractfile(f"blobs/{algo}/{hexd}"))
        index = json.load(tar.extractfile("index.json"))
        manifests = index.get("manifests", [])
        if len(manifests) != 1:
            raise SystemExit(f"{path}: {len(manifests)} images in it, not one")
        manifest = blob(manifests[0]["digest"])
        return manifest["config"]["digest"].partition(":")[2]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--image-file", required=True, type=Path)
    ap.add_argument("--version", required=True)
    ap.add_argument("--commit", required=True)
    ap.add_argument("--channel", choices=("stable", "experimental"))
    ap.add_argument("--steamos", type=Path, default=HERE / "steamos.json")
    a = ap.parse_args()
    if not re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}", a.version):
        ap.error(f"--version is malformed: {a.version!r}")
    if not re.fullmatch(r"[0-9a-f]{7,40}", a.commit):
        ap.error(f"--commit is malformed: {a.commit!r}")
    table = json.loads(a.steamos.read_text())
    json.dump({
        "schema": "frametop.release/v1",
        "version": a.version,
        "commit": a.commit,
        "channel": a.channel or ("experimental" if "-" in a.version else "stable"),
        "image": {"file": a.image_file.name, "sha256": sha256(a.image_file), "id": image_id(a.image_file)},
        "steamos": {"tested": table.get("tested", []), "broken": table.get("broken", [])},
    }, sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
