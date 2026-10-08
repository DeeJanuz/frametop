#!/usr/bin/env bash
# Frametop's installer, in a release's Frametop.zip: the start command of the "Frametop" title
# FrameDrop puts in the Steam library, or what you run after unpacking the zip on the headset.
# It opens the install window (progress.py), which asks what to install, and your password
# for the parts that need sudo, then installs Frametop (or updates it) in a service of its own
# and shows its progress. A release's zip installs its own image, built
# (install-release.sh); a test zip clones Frametop from GitHub (get.sh).
#
# Usage: frametop-install.sh [--dry-run]
#   --dry-run  unpack into ~/.cache/frametop-framedrop/dry-run and stop there, without
#              installing, for testing this flow
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
