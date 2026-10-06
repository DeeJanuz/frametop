#!/usr/bin/env bash
# Build the ft-pointer helper on the Frame, in the dev container (it also runs there).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C pointer/helper 'set -e; mkdir -p build
. ../../scripts/openvr.sh
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter $OPENVR_CFLAGS -I../common \
  -o build/ft-pointer ft-pointer.cpp $OPENVR_LIBS -lpthread
echo "built build/ft-pointer"'
