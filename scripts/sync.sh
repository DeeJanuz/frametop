#!/usr/bin/env bash
# One-way sync of this repo from a PC to ~/dev/frametop on the Steam Frame.
# Usage: scripts/sync.sh [extra rsync args, e.g. --dry-run]
# Honors .gitignore files and skips .git (dirs, and the files submodules use), build outputs, and .env files.
# --delete only removes files inside ~/dev/frametop on the headset.
# On the Frame itself there's nothing to do: the checkout is used directly.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
if [ "$FRAME_LOCAL" = 1 ]; then
  exit 0
fi

dest=${FRAME_REPO#/home/steamos/}  # relative paths start in the home folder over SSH
# rsync creates only the last component of the destination path, and a fresh Frame
# has no ~/dev, so the first sync from a PC would fail. The remote rsync makes it first,
# in the same SSH connection (--mkdir needs rsync 3.2.3 on the PC too).
exec rsync -az --delete --info=stats1 \
  --rsync-path="mkdir -p $(printf %q "$dest") && rsync" \
  --filter=':- .gitignore' \
  --exclude='.git' --exclude='target/' --exclude='build/' --exclude='.env' --exclude='.env.*' \
  "$@" "$root/" "$FRAME_HOST:$dest/"
