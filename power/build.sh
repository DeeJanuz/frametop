#!/usr/bin/env bash
# Build ft-powerd on the Frame, in the dev container (it also runs there).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C power 'set -e; mkdir -p build
. ../scripts/openvr.sh
g++ -std=c++17 -O2 -Wall -Wno-unused-parameter $OPENVR_CFLAGS \
  -o build/ft-powerd ft-powerd.cpp $OPENVR_LIBS
echo "built build/ft-powerd"'
