#!/usr/bin/env bash
# Build stream/build/ft-stream (ft-stream.cpp and host.cpp: a remote display's stream, for
# ft-screens) in the dev container on the Frame, and the remote-display spikes:
# stream/build/ft-dectest (spike S1, spike/ft-dectest.cpp) and stream/build/ft-streamtest
# (spike S3, spike/ft-streamtest.cpp). Uses the same pinned OpenVR header as ft-screens, for
# IVRIPCResourceManagerClient::ImportDmabuf. ft-stream and ft-streamtest also need moonlight-embedded
# (GPLv3) at a pinned commit: its libgamestream for pairing and launching, and the
# moonlight-common-c it pins for the protocol, and its table of Windows key codes. ft-stream
# plays the host's sound with Opus and PipeWire's PulseAudio server (libpulse-simple).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C stream 'set -e; mkdir -p build/include
openvr=v2.15.6
[ -f build/include/openvr-$openvr ] || { curl -fsSL "https://raw.githubusercontent.com/ValveSoftware/openvr/$openvr/headers/openvr.h" -o build/include/openvr.h && touch build/include/openvr-$openvr; }
me=f32e415aea6797d261d6b470dcf8bf18727341c2
src=build/moonlight-embedded
mlc=$src/third_party/moonlight-common-c
if [ ! -f $src/.ft-$me ]; then
  rm -rf $src build/mlc build/gamestream
  git clone -q https://github.com/moonlight-stream/moonlight-embedded.git $src
  git -C $src checkout -q $me
  git -C $src submodule update -q --init third_party/moonlight-common-c
  git -C $mlc submodule update -q --init enet
  touch $src/.ft-$me
fi
if [ ! -f build/mlc/libmoonlight-common-c.a ]; then
  cmake -S $mlc -B build/mlc -G Ninja -DBUILD_SHARED_LIBS=OFF -DCMAKE_BUILD_TYPE=Release >/dev/null
  ninja -C build/mlc >/dev/null
fi
mkdir -p build/gamestream
for f in client http mkcert xml; do
  [ build/gamestream/$f.o -nt $src/libgamestream/$f.c ] ||
    gcc -O2 -w -I$mlc/src -c $src/libgamestream/$f.c -o build/gamestream/$f.o
done
flags="-std=c++17 -O2 -Wall -Wno-missing-field-initializers -Ibuild/include $(pkg-config --cflags gbm libdrm egl glesv2)"
vr="-L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64"
g++ $flags -o build/ft-dectest spike/ft-dectest.cpp $(pkg-config --libs gbm egl glesv2) $vr
echo "built build/ft-dectest"
g++ $flags -I$mlc/src -I$src/libgamestream -o build/ft-streamtest spike/ft-streamtest.cpp build/gamestream/*.o \
  build/mlc/libmoonlight-common-c.a build/mlc/enet/libenet.a \
  $(pkg-config --libs gbm egl glesv2 libcurl openssl expat uuid) -lpthread $vr
echo "built build/ft-streamtest"
g++ $flags -I$mlc/src -I$src/libgamestream -I$src/src -o build/ft-stream ft-stream.cpp host.cpp build/gamestream/*.o \
  build/mlc/libmoonlight-common-c.a build/mlc/enet/libenet.a \
  $(pkg-config --libs gbm egl glesv2 libcurl openssl expat uuid opus libpulse-simple) -lpthread $vr
echo "built build/ft-stream"'
