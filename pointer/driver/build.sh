#!/usr/bin/env bash
# Build the ft_pointer SteamVR driver on the Frame (dev container) and check it
# only needs glibc symbols the SteamOS host has (2.39; the container has 2.43).
# -fno-math-errno keeps sqrtf inline. libm's float functions (sqrtf, atan2f, asinf,
# remainderf) are versioned GLIBC_2.43 here, so the driver uses the double versions.
# Usage: pointer/driver/build.sh
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
"$root/scripts/sync.sh" >/dev/null
exec "$root/scripts/frame.sh" -C pointer/driver 'set -e
mkdir -p build
. ../../scripts/openvr.sh
g++ -std=c++17 -O2 -fPIC -shared -fvisibility=hidden -fno-math-errno -Wall -Wno-unused-parameter \
  -static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL \
  $OPENVR_CFLAGS \
  -o build/driver_ft_pointer.so driver_ft_pointer.cpp -lpthread
max=$(objdump -T build/driver_ft_pointer.so | grep -oE "GLIBC_[0-9.]+" | sort -uV | tail -1)
echo "built build/driver_ft_pointer.so, newest glibc symbol: $max"
[ "$(printf "%s\n" "$max" GLIBC_2.39 | sort -V | tail -1)" = GLIBC_2.39 ] || { echo "needs newer glibc than the host has" >&2; exit 1; }'
