# just — the runner for the actions that happen inside the image.
# The CI smoke job runs these with: docker run ... IMAGE just test
# Host-side image management (build, shell, update) lives in ./ft; these
# recipes only ever run where the toolchain and the venv are: in the image.

# default: list the recipes
default:
    @just --list

# everything, strict (the recipes are the source of truth)
test: test-python test-c test-bash

# the Python suites as a strict gate: every failure fails the run. The image's
# python3 has to import the dnf Qt stack and the locked packages together;
# without that check the Qt tests would only skip. session/test is not here:
# test_accessibility.py needs the host's AT-SPI and PyGObject.
test-python:
    #!/usr/bin/env bash
    set -uo pipefail
    python3 -c 'import PySide6, numpy, cv2' || { echo "python3 can't import PySide6, numpy, and cv2"; exit 1; }
    status=0
    for t in input/test/*.py hands/tests/test_*.py hands/rec/tests/*.py gaze/test/*.py pack/test/*.py; do
        [ -f "$t" ] || continue
        if python3 "$t" >/dev/null; then echo "$t: ok"; else echo "$t: FAILED"; status=1; fi
    done
    pytest -q session/tests || status=1
    exit $status

# the C/C++ unit tests that need no model runtime: header-only logic with
# made-up events. sides_test.cpp is NOT here: it pulls in ncnn (hands'
# deferred heavy build) and stays with `make check` in the hands build
# (hands/Makefile).
test-c:
    #!/usr/bin/env bash
    set -e
    mkdir -p build/tests
    gcc -std=c11 -Wall -Wextra -Werror -I. screens/tests/controller-click-test.c \
      -lm -o build/tests/controller-click-test
    build/tests/controller-click-test && echo "screens/tests/controller-click-test.c: ok"
    gcc -std=c11 -Wall -Wextra -Werror screens/tests/relay-buttons-test.c \
      -o build/tests/relay-buttons-test
    build/tests/relay-buttons-test && echo "screens/tests/relay-buttons-test.c: ok"

# shell scripts: syntax-gate everything that ships. No git: the image has
# none, and a mounted checkout can trip "dubious ownership" — find lists
# what the repo actually ships. The count guard makes a silently short
# list fail the gate instead of passing it.
test-bash:
    #!/usr/bin/env bash
    set -e
    mapfile -t scripts < <(find . -name '*.sh' -type f \
        -not -path './.git/*' -not -path '*/build/*' | sort)
    [ "${#scripts[@]}" -ge 20 ] || { echo "only ${#scripts[@]} scripts found — the list is broken"; exit 1; }
    status=0
    for f in "${scripts[@]}" ft; do
        bash -n "$f" && echo "bash -n $f: ok" || status=1
    done
    exit $status

# lint the Python side (ruff from the locked dev group). Report only for now:
# the codebase has pre-existing findings; making it a gate is its own cleanup.
lint:
    -ruff check .

# the whole gate: lint, then tests
check: lint test
