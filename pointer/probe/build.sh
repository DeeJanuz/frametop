#!/usr/bin/env bash
# Build vrprobe, lasertest, vrsetting and focustest in the dev container on the Frame (pointer/probe/build/vrprobe).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C pointer/probe 'set -e; mkdir -p build
g++ -std=c++17 -O2 -Wall -I/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers -I../common \
  -o build/vrprobe vrprobe.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64
g++ -std=c++17 -O2 -Wall -I/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers \
  -o build/lasertest lasertest.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64
g++ -std=c++17 -O2 -Wall -I/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers \
  -o build/vrsetting vrsetting.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64
g++ -std=c++17 -O2 -Wall -I/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers \
  -o build/focustest focustest.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64
echo "built build/vrprobe build/lasertest build/vrsetting build/focustest"'
