#!/usr/bin/env bash
# Runs on the Frame host, from install.sh in a release (get.sh --release): make the release's
# container from its image, as setup/dev-container.sh makes "dev" for a source install. Both
# come from .frametop-release: IMAGE (by digest, pulled already) and BOX, the container's name,
# which scripts/in-box runs the release's programs in. Each release gets a container of its
# own, so installing one never stops the programs of the one running now; get.sh removes the
# containers of releases it removes.
# Usage: pack/release-box.sh
set -euo pipefail
tree=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
image=$(sed -n 's/^IMAGE=//p' "$tree/.frametop-release" | tail -1)
box=$(sed -n 's/^BOX=//p' "$tree/.frametop-release" | tail -1)
[ -n "$image" ] && [[ $box =~ ^frametop-[A-Za-z0-9_.-]+$ ]] ||
  { echo "$tree/.frametop-release doesn't name an image and a frametop-... container" >&2; exit 1; }
export XDG_RUNTIME_DIR=/run/user/$(id -u)
if podman container exists "$box"; then
  echo "the $box container is there already"
else
  podman image exists "$image" || podman pull "$image"
  echo "creating the $box container"
  # --no-entry: no "enter this container" entry among the apps (Launch a program lists them).
  "$HOME/.local/bin/distrobox" create --yes --no-entry --name "$box" --image "$image"
fi
"$tree/scripts/container-up.sh" "$box"
echo "$box ready: $(podman container inspect -f '{{.ImageName}}' "$box")"
