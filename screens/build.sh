#!/usr/bin/env bash
# Build ft-screens in the dev container on the Frame (screens/build/ft-screens), and
# ft-handtest, which tries the hand cutouts (handcut.cpp) on a test panel of its own.
# compositor.c is the wlroots side (C; wlroots headers aren't C++), vr.cpp the OpenVR side,
# remote.c the remote screens (their streams are ft-stream's, stream/build.sh).
# vr.cpp needs OpenVR's IVRIPCResourceManagerClient (ImportDmabuf), which the header
# shipped with SteamVR on the Frame predates, so the build uses the public header from
# Valve's openvr repo (pinned in scripts/openvr.sh).
# keyboard.cpp draws its key labels with stb_truetype (public domain, one header, pinned).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C screens 'set -e; mkdir -p build/include
. ../scripts/openvr.sh
stb=2c980bb59875b0d32144a71867fbdebb2f77cd20
[ -f build/include/stb-$stb ] || { curl -fsSL "https://raw.githubusercontent.com/nothings/stb/$stb/stb_truetype.h" -o build/include/stb_truetype.h && touch build/include/stb-$stb; }
cc="gcc -std=c11 -O2 -Wall -Wno-unused-parameter $(pkg-config --cflags wlroots-0.20 wayland-server xkbcommon libdrm pixman-1)"
$cc -c -o build/compositor.o compositor.c
$cc -c -o build/remote.o remote.c
cxx="g++ -std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags egl glesv2 gbm libdrm)"
$cxx -c -o build/vr.o vr.cpp
$cxx -c -o build/keyboard.o keyboard.cpp
$cxx -c -o build/handcut.o handcut.cpp
$cxx -c -o build/handtest.o handtest.cpp
vrlibs="$(pkg-config --libs egl glesv2 gbm) $OPENVR_LIBS"
g++ -o build/ft-screens build/compositor.o build/remote.o build/vr.o build/keyboard.o build/handcut.o \
  $(pkg-config --libs wlroots-0.20 wayland-server xkbcommon pixman-1) $vrlibs
g++ -o build/ft-handtest build/handtest.o build/handcut.o $vrlibs
echo "built build/ft-screens build/ft-handtest"'
