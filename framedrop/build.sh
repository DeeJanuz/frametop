#!/usr/bin/env bash
# Build the FrameDrop download: framedrop/build/Frametop.zip (the installer in a "Frametop"
# folder) and framedrop/build/frametop.framedrop.json, the manifest an "Install with
# FrameDrop" button points at. The zip is reproducible: same files, same sha256.
#
# Usage: framedrop/build.sh [ZIP_URL]
#   ZIP_URL  where the zip will be downloaded from (default: the framedrop-installer release)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
url=${1:-https://github.com/DeeJanuz/frametop/releases/download/framedrop-installer/Frametop.zip}
out=$here/build
mkdir -p "$out"

python3 - "$here/installer" "$out/Frametop.zip" <<'EOF'
import os, sys, zipfile
src, dest = sys.argv[1:]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for name in ("frametop-install.sh", "progress.py"):
        info = zipfile.ZipInfo(f"Frametop/{name}", date_time=(2026, 1, 1, 0, 0, 0))
        info.create_system = 3  # unix, so the permissions below count
        info.external_attr = (0o100755 if os.access(os.path.join(src, name), os.X_OK) else 0o100644) << 16
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(os.path.join(src, name), "rb") as f:
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
echo "built $out/Frametop.zip ($sha)"
echo "      $out/frametop.framedrop.json"
