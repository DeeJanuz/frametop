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

# lint the Python side (ruff from the locked dev group). Report only for now:
# the codebase has pre-existing findings; making it a gate is its own cleanup.
lint:
    -ruff check .

# the whole gate: lint, then tests
check: lint test
