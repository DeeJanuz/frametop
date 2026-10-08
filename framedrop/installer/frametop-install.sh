#!/usr/bin/env bash
# Frametop's FrameDrop installer: the start command of the "Frametop" title that FrameDrop
# puts in the Steam library. Playing it installs Frametop (or updates it) with the one-line
# installer, get.sh --yes, and shows its progress in a window.
#
# Steam ends a title's whole process tree when it's quit, and starts it with a high OOM score,
# so the install runs in a user service of its own (frametop-framedrop-install) and the window
# only follows its log. Quitting or closing the window leaves the install running; playing the
# title again reattaches to it.
#
# Usage: frametop-install.sh [--dry-run]
#   --dry-run  clone into ~/.cache/frametop-framedrop/dry-run instead of ~/frametop, and
#              don't run install.sh (get.sh --clone-only), for testing this flow
set -u

here=$(cd "$(dirname "$0")" && pwd)
self=$here/$(basename "$0")

# A Linux title can be started in the Steam Linux Runtime container, which has no git, podman,
# systemctl, or GTK. Start this again on the host. (SteamOS 0.3.0 runs devkit titles on the
# host anyway; this is for when FrameDrop or Steam picks the runtime.)
if [ -e /run/pressure-vessel ]; then
  exec flatpak-spawn --host --watch-bus --env=DISPLAY="${DISPLAY-}" \
    --env=XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR-}" --env=SteamAppId="${SteamAppId-}" \
    --env=ENABLE_GAMESCOPE_WSI="${ENABLE_GAMESCOPE_WSI-}" /usr/bin/bash "$self" "$@"
fi

# Steam adds its overlay and runtime libraries to every child; host tools don't want them.
unset LD_PRELOAD LD_LIBRARY_PATH
export PATH=/usr/local/bin:/usr/bin:/bin

dry_run=0
[ "${1-}" = --dry-run ] && dry_run=1

unit=frametop-framedrop-install
state=$HOME/.cache/frametop-framedrop
log=$state/install.log
mkdir -p "$state"

get_args="--yes"
[ "$dry_run" = 1 ] && get_args="--yes --clone-only --dir $state/dry-run"

# Start the install unless one is running already. RemainAfterExit keeps its result for the
# window; the next start clears it.
if [ "$(systemctl --user show -P ActiveState "$unit" 2>/dev/null)" != activating ]; then
  systemctl --user stop "$unit" 2>/dev/null
  systemctl --user reset-failed "$unit" 2>/dev/null
  : >"$log"
  systemd-run --user --unit="$unit" --description="Frametop install (FrameDrop)" \
    --property=Type=oneshot --property=RemainAfterExit=yes \
    --property=StandardOutput="truncate:$log" --property=StandardError=inherit \
    --setenv=HOME="$HOME" --setenv=PATH="$PATH" --setenv=TERM=dumb \
    --working-directory="$HOME" --quiet --no-block \
    /usr/bin/bash -c 'set -o pipefail; curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash -s -- '"$get_args" ||
    { echo "couldn't start the install service" >&2; exit 1; }
fi

exec /usr/bin/python3 "$here/progress.py" "$unit" "$log"
