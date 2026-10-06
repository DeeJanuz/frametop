#!/usr/bin/env bash
# Offline test of ft-gaze's eye-server.mmap layout detection (gaze/test/mmap-layout-test.cpp):
# builds it against gaze/ft-gaze.cpp in the dev container and runs it there. Nothing reaches
# SteamVR or the eye tracker, so it's safe next to them. gaze/build.sh fetches the OpenVR
# header it needs, so run that once first.
#
#   gaze/test/mmap-layout-test.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C gaze 'set -e
[ -f build/include/openvr.h ] || { echo "no build/include/openvr.h: run gaze/build.sh first" >&2; exit 1; }
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -Wno-missing-field-initializers -Ibuild/include -I../pointer/common \
  -o build/mmap-layout-test test/mmap-layout-test.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api \
  -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -lpthread
build/mmap-layout-test'
