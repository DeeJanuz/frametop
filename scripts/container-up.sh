#!/bin/bash
# Runs on the Frame host. Starts the dev container, if it isn't running, in a systemd scope
# of its own. Whoever starts a podman container owns its monitor (conmon): started from one
# of our services (distrobox enter starts it on demand), the container, and everything in it
# like the desktop's compositor, would be killed when that service stops. Call this before
# `distrobox enter` (scripts/in-box does).
# Usage: scripts/container-up.sh [BOX]   (default: FRAME_BOX, else "dev")
box=${1:-${FRAME_BOX:-dev}}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
running() { [ "$(podman container inspect -f '{{.State.Running}}' "$box" 2>/dev/null)" = true ]; }
running && exit 0
podman container exists "$box" 2>/dev/null || exit 0  # not created yet: setup/dev-container.sh does that
since=$(date -u +%FT%T)
systemd-run --user --scope --quiet --collect --description="$box container (started for Frametop)" \
  podman start "$box" >/dev/null || exit

# Every start runs distrobox-init in the container, and `distrobox enter` only waits for it
# when it starts the container itself. The first start installs what distrobox needs and sets
# up passwordless sudo, which takes a minute or more; until then sudo in the container asks
# for a password (issue #9). Later starts take a few seconds.
for i in $(seq 600); do
  podman logs --since "$since" "$box" 2>&1 | grep -q '^container_setup_done' && exit 0
  running || break
  [ "$i" = 10 ] && echo "setting up the $box container (the first start takes a few minutes)" >&2
  sleep 1
done
echo "the $box container didn't finish starting; see: podman logs $box" >&2
exit 1
