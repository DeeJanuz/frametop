#!/usr/bin/env bash
# Build our own eye tracker on the Frame, in the dev container:
#   build/ft-eyegrab   the frame grabber. It runs on the host, as root
#                      (frametop-eyegrab.service, gaze/tracker/install.sh), so this checks it
#                      only needs glibc symbols the SteamOS host has (2.39; the container has 2.43).
#   build/venv         Python with numpy and OpenCV (requirements.txt) for ft-eyes and lab/,
#                      remade when requirements.txt changes. In the image (pack/Containerfile)
#                      they're in its locked venv already (uv.lock has the same versions), so
#                      build/venv is a small venv that sees that one's packages, not a copy.
# Usage: gaze/tracker/build.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C gaze/tracker 'set -e; mkdir -p build
gcc -std=gnu11 -O2 -Wall -Wextra -pthread -o build/ft-eyegrab ft-eyegrab.c
max=$(objdump -T build/ft-eyegrab | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1)
echo "built build/ft-eyegrab, newest glibc symbol: $max"
[ "$(printf "%s\n" "$max" GLIBC_2.39 | sort -V | tail -1)" = GLIBC_2.39 ] || { echo "needs newer glibc than the host has" >&2; exit 1; }
if [ "${FRAME_IN_BOX:-0}" = 1 ] && [ -x /opt/frametop/venv/bin/python ]; then
  rm -rf build/venv
  python3 -m venv --without-pip build/venv
  /opt/frametop/venv/bin/python -c "import site; print(site.getsitepackages()[0])" \
    >"$(build/venv/bin/python -c "import site; print(site.getsitepackages()[0])")/frametop-image.pth"
elif ! cmp -s requirements.txt build/venv/requirements.done; then
  rm -rf build/venv
  python3 -m venv build/venv
  build/venv/bin/pip install -q --disable-pip-version-check -r requirements.txt
  cp requirements.txt build/venv/requirements.done
fi
echo "build/venv: $(build/venv/bin/python -c "import numpy, cv2; print(\"numpy\", numpy.__version__, \"opencv\", cv2.__version__)")"'
