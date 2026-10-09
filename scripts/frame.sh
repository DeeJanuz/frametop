#!/usr/bin/env bash
# Run a command on the Steam Frame, inside the "dev" distrobox by default.
# On the Frame itself it runs locally in this checkout; from a PC it runs over SSH
# in the synced copy (see scripts/_env.sh).
# Usage: scripts/frame.sh [--host] [-C subdir] command [args...]
#   --host      run on the SteamOS host instead of the container
#   -C subdir   start in <repo>/<subdir> (default: the repo root)
# The command is passed to a shell as one string, like ssh does.
set -euo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/_env.sh"

on_host=0
sub=
while [ $# -gt 0 ]; do
  case $1 in
    --host) on_host=1; shift ;;
    -C) sub=$2; shift 2 ;;
    --) shift; break ;;
    *) break ;;
  esac
done
[ $# -gt 0 ] || { echo "usage: $0 [--host] [-C subdir] command [args...]" >&2; exit 2; }
dir=$FRAME_REPO${sub:+/$sub}

# Already in the build container (FRAME_IN_BOX=1, the image build): no distrobox to enter.
if [ "${FRAME_IN_BOX:-0}" = 1 ] && [ "$on_host" = 0 ]; then
  cd "$dir"
  exec bash -lc "$*"
fi

# distrobox is called by its full path: ~/.bashrc on the Frame returns early for
# non-interactive shells, so ~/.local/bin isn't on PATH. distrobox enter keeps the cwd.
if [ "$FRAME_LOCAL" = 1 ]; then
  cd "$dir"
  if [ "$on_host" = 1 ]; then
    exec bash -c "$*"
  fi
  "$REPO_ROOT/scripts/container-up.sh" "$FRAME_BOX"  # in a scope of its own, not this shell's session
  exec "$HOME/.local/bin/distrobox" enter "$FRAME_BOX" -- bash -lc "$*"
fi

tty=()
[ -t 0 ] && [ -t 1 ] && tty=(-t)
if [ "$on_host" = 1 ]; then
  exec ssh "${tty[@]}" "$FRAME_HOST" "cd $(printf %q "$dir") && $*"
else
  exec ssh "${tty[@]}" "$FRAME_HOST" \
    "$(printf %q "$FRAME_REPO")/scripts/container-up.sh $(printf %q "$FRAME_BOX"); cd $(printf %q "$dir") && ~/.local/bin/distrobox enter $(printf %q "$FRAME_BOX") -- bash -lc $(printf %q "$*")"
fi
