#!/usr/bin/env bash
# Runs on the Frame host, from install.sh in a release (pack/install-release.sh): make the
# release's container from its image, as setup/dev-container.sh makes "dev" for a source
# install. Both come from .frametop-release: IMAGE (localhost/frametop:VERSION, loaded from the
# release's image file already), IMAGE_ID (checked), and BOX, the container's name, which
# scripts/in-box runs the release's programs in. Each release gets a container of its own, so
# installing one never stops the programs of the one running now; install-release.sh removes
# the containers of releases it removes.
# Usage: pack/release-box.sh
set -euo pipefail
tree=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
field() { sed -n "s/^$1=//p" "$tree/.frametop-release" | tail -1; }
image=$(field IMAGE)
id=$(field IMAGE_ID)
box=$(field BOX)
[[ $image == localhost/frametop:* ]] && [[ $id =~ ^[0-9a-f]{64}$ ]] && [[ $box =~ ^frametop-[A-Za-z0-9_.-]+$ ]] ||
  { echo "$tree/.frametop-release doesn't name a localhost/frametop image, its ID, and a frametop-... container" >&2; exit 1; }
export XDG_RUNTIME_DIR=/run/user/$(id -u)
[ "$(podman image inspect -f '{{.Id}}' "$image" 2>/dev/null)" = "$id" ] ||
  { echo "$image isn't the release's image (load it with the release's install-release.sh)" >&2; exit 1; }
if podman container exists "$box"; then
  echo "the $box container is there already"
else
  echo "creating the $box container"
  # --no-entry: no "enter this container" entry among the apps (Launch a program lists them).
  "$HOME/.local/bin/distrobox" create --yes --no-entry --name "$box" --image "$image"
fi
"$tree/scripts/container-up.sh" "$box"
echo "$box ready: $image"
