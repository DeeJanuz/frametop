#!/usr/bin/env bash
# Frametop's FrameDrop installer: the start command of the "Frametop" title that FrameDrop
# puts in the Steam library. Playing it opens the install window (progress.py): it asks what
# to install, and your password for the parts that need sudo, then installs Frametop (or
# updates it) with get.sh --yes in a service of its own, and shows its progress. With a
# release list next to it (frametop-releases.json), it installs that release, built, from its
# image (get.sh --release); without one, it clones Frametop from GitHub.
#
# Usage: frametop-install.sh [--dry-run]
#   --dry-run  get the files into ~/.cache/frametop-framedrop/dry-run and stop there
#              (get.sh --clone-only), for testing this flow
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

exec /usr/bin/python3 "$here/progress.py" "$here" "$@"
