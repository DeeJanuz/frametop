#!/usr/bin/env bash
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
"$root/scripts/frame.sh" -C screens 'mkdir -p build; gcc -std=c11 -Wall -Wextra -Werror tests/controller-click-test.c -lm -o build/controller-click-test && build/controller-click-test'
