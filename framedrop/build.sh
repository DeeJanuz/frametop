#!/usr/bin/env bash
# Build the FrameDrop download: framedrop/build/Frametop.zip (the installer in a "Frametop"
# folder, with this checkout's get.sh) and framedrop/build/frametop.framedrop.json, the
# manifest an "Install with FrameDrop" button points at. The zip is reproducible: same files,
# same sha256.
#
# Usage: framedrop/build.sh [--releases FILE] [ZIP_URL]
#   --releases FILE  a release list (pack/release-manifest.py) to put in the zip: the
#                    installer then installs that release, built, from its image. Without
#                    one, it clones Frametop from GitHub, as get.sh does.
#   ZIP_URL  where the zip will be downloaded from (default: the framedrop-installer release)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
releases=
if [ "${1-}" = --releases ]; then
  releases=$(cd "$(dirname "${2:?--releases needs a file}")" && pwd)/$(basename "$2")
  shift 2
fi
url=${1:-https://github.com/DeeJanuz/frametop/releases/download/framedrop-installer/Frametop.zip}
out=$here/build
mkdir -p "$out"

python3 - "$here/installer" "$here/../get.sh" "$releases" "$out/Frametop.zip" <<'EOF'
import os, sys, zipfile
src, get_sh, releases, dest = sys.argv[1:]
files = [(os.path.join(src, n), n) for n in ("frametop-install.sh", "progress.py", "askpass")]
files.append((get_sh, "get.sh"))
if releases:
    files.append((releases, "frametop-releases.json"))
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for path, name in files:
        info = zipfile.ZipInfo(f"Frametop/{name}", date_time=(2026, 1, 1, 0, 0, 0))
        info.create_system = 3  # unix, so the permissions below count
        info.external_attr = (0o100755 if os.access(path, os.X_OK) else 0o100644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(path, "rb") as f:
            z.writestr(info, f.read())
EOF

sha=$(sha256sum "$out/Frametop.zip" | cut -d' ' -f1)
python3 - "$url" "$sha" >"$out/frametop.framedrop.json" <<'EOF'
import json, sys
url, sha = sys.argv[1:]
print(json.dumps({
    "schema": "framedrop.install/v1",
    "name": "Frametop",
    "files": [{"url": url, "sha256": sha}],
}, indent=2))
EOF
echo "built $out/Frametop.zip ($sha)${releases:+, installing a release from $releases}"
echo "      $out/frametop.framedrop.json"
