#!/usr/bin/env bash
# Offline native input tests; --headless also starts an isolated test desktop.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
[[ $# == 0 || ($# == 1 && $1 == --headless) ]] || { echo 'Usage: scripts/test-desktop-input.sh [--headless]' >&2; exit 2; }
python3 -m unittest discover -s input -p 'test_desktop_mouse.py'
"$root/scripts/frame.sh" -C screens 'set -e; mkdir -p build
gcc -std=c11 -Wall -Wextra -Werror tests/desktop-mouse-test.c -lm -o build/desktop-mouse-test
build/desktop-mouse-test
gcc -std=c11 -Wall -Wextra -Werror tests/controller-fallback-test.c -o build/controller-fallback-test
build/controller-fallback-test
gcc -std=c11 -Wall -Wextra -Werror tests/desktop-handoff-test.c -o build/desktop-handoff-test
build/desktop-handoff-test
gcc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) tests/desktop-cursor-test.c -o build/desktop-cursor-test
build/desktop-cursor-test
gcc -std=c11 -Wall -Wextra -Werror tests/cursor-cache-test.c $(pkg-config --cflags --libs wlroots-0.20 wayland-server wayland-client libdrm pixman-1) -o build/cursor-cache-test
build/cursor-cache-test'
if [[ ${1:-} == --headless ]]; then
  # Reuses upstream's throwaway @ft_screens_test fixture, never the live desktop.
  if pgrep -f '[f]t-screens.*--control ft_screens_test' >/dev/null; then
    echo 'Test desktop already running; stop it yourself before this test.' >&2
    exit 1
  fi
  "$root/screens/build.sh"
  trap '"$root/screens/test/headless.sh" stop' EXIT
  "$root/screens/test/headless.sh" start 2 3
  python3 "$root/screens/tests/desktop-input-integration.py"
fi
