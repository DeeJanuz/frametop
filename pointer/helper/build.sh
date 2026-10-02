#!/usr/bin/env bash
# Build the ft-pointer helper on the Frame, in the dev container (it also runs there).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C pointer/helper 'set -e; mkdir -p build
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter -I/opt/steamvr/tools/hellovr_vulkan_linux/src/openvr/headers -I../common \
  -o build/ft-pointer ft-pointer.cpp pointer_*.cpp -L/opt/steamvr/bin/linuxarm64 -lopenvr_api -Wl,-rpath,/opt/steamvr/bin/linuxarm64 -lpthread
echo "built build/ft-pointer"'
