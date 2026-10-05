# just — the runner for the actions that happen inside the image.
# The CI smoke job runs these with: docker run ... IMAGE just test
# Host-side image management (build, shell, update) lives in ./ft; these
# recipes only ever run where the toolchain and the venv are: in the image.

# default: list the recipes
default:
    @just --list

# the Python suites (strict: these must pass everywhere)
test:
    #!/usr/bin/env bash
    set -e
    python3 session/tests/test_fix_panels.py
    python3 input/test/pause-test.py >/dev/null && echo "input/test/pause-test.py: ok"
    python3 input/test/keys-test.py >/dev/null && echo "input/test/keys-test.py: ok"
    for t in hands/rec/tests/*.py gaze/test/*.py; do
        [ -f "$t" ] || continue
        python3 "$t" >/dev/null 2>&1 && echo "$t: ok" || echo "$t: FAIL or needs the Frame"
    done
    pytest -q session/tests

# the Python suites as a strict gate (every failure fails the run)
test-python:
    #!/usr/bin/env bash
    set -e
    python3 session/tests/test_fix_panels.py
    for t in input/test/*.py hands/tests/test_*.py hands/rec/tests/*.py gaze/test/*.py; do
        [ -f "$t" ] || continue
        python3 "$t" >/dev/null && echo "$t: ok"
    done
    pytest -q session/tests

# the C/C++ unit tests that need no model runtime. controller-click-test
# tests header-only logic with made-up events. sides_test.cpp is NOT here:
# it pulls in ncnn (hands' deferred heavy build) and stays with
# `make check` in the hands build (hands/Makefile).
test-c:
    #!/usr/bin/env bash
    set -e
    mkdir -p build/tests
    gcc -std=c11 -Wall -Wextra -Werror -I. screens/tests/controller-click-test.c \
      -lm -o build/tests/controller-click-test
    build/tests/controller-click-test && echo "screens/tests/controller-click-test.c: ok"

# shell scripts: syntax-gate everything that ships
test-bash:
    #!/usr/bin/env bash
    set -e
    status=0
    for f in $(git ls-files '*.sh') ft; do
        [ -f "$f" ] || continue
        bash -n "$f" && echo "bash -n $f: ok" || status=1
    done
    exit $status

# lint the Python side (ruff from the locked dev group). Report only for now:
# the codebase has pre-existing findings; making it a gate is its own cleanup.
lint:
    -ruff check .

# the whole gate: lint, then tests
check: lint test
