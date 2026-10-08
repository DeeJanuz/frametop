#!/usr/bin/env python3
"""Write a release list (frametop.release/v1), the file get.sh --release installs from.

    pack/release-manifest.py --channel experimental --version 0.3.0-exp.6dc4521 \\
        --commit 6dc4521... --image ghcr.io/deejanuz/frametop@sha256:... \\
        [--previous releases/experimental.json] > releases/experimental.json

The new release goes first (get.sh picks the newest that fits), then up to --keep - 1 from
--previous. Every release in the list gets the SteamOS table from pack/steamos.json as it is
now, so a build tested after a release came out counts for it too. A broken build counts for
the releases from its "from" (when a release started needing something that build lacks) up
to its "fixed_in", either one open. With no --version, it only refreshes --previous's tables.
"""
import argparse
import json
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
VERSION = re.compile(r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}")
IMAGE = re.compile(r"[a-z0-9][a-z0-9._/:-]{0,200}@sha256:[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{7,40}")


def base(version):
    """0.3.0-exp.6dc4521 -> (0, 3, 0): the release it leads up to."""
    return tuple(int(p) for p in re.findall(r"\d+", version.split("-")[0].split("+")[0]))


def table(version, steamos):
    broken = [b for b in steamos.get("broken", [])
              if (not b.get("from") or base(version) >= base(b["from"]))
              and (not b.get("fixed_in") or base(version) < base(b["fixed_in"]))]
    gone = {b.get("build") for b in broken}  # tested with an older release, broken for this one
    return {"tested": [t for t in steamos.get("tested", []) if t.get("build") not in gone], "broken": broken}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--channel", required=True, choices=("stable", "experimental"))
    ap.add_argument("--version")
    ap.add_argument("--commit")
    ap.add_argument("--image")
    ap.add_argument("--previous", type=Path)
    ap.add_argument("--steamos", type=Path, default=HERE / "steamos.json")
    ap.add_argument("--keep", type=int, default=5)
    a = ap.parse_args()

    releases = []
    if a.version:
        for name, value, pattern in (("version", a.version, VERSION), ("commit", a.commit, COMMIT),
                                     ("image", a.image, IMAGE)):
            if not value or not pattern.fullmatch(value):
                ap.error(f"--{name} is missing or malformed: {value!r}")
        releases.append({"version": a.version, "commit": a.commit, "image": a.image})
    elif not a.previous:
        ap.error("--version (with --commit and --image), or --previous to refresh")
    if a.previous and a.previous.exists():
        old = json.loads(a.previous.read_text())
        if old.get("schema") != "frametop.release/v1":
            ap.error(f"{a.previous} isn't a frametop.release/v1 list")
        releases += [{k: r[k] for k in ("version", "commit", "image")}
                     for r in old.get("releases", []) if r.get("version") != a.version]
    releases = releases[:a.keep]

    steamos = json.loads(a.steamos.read_text())
    for r in releases:
        r["steamos"] = table(r["version"], steamos)
    json.dump({"schema": "frametop.release/v1", "channel": a.channel, "releases": releases},
              sys.stdout, indent=2)
    print()


if __name__ == "__main__":
    main()
