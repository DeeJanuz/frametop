#!/usr/bin/env bash
# Install (or remove) Frametop Remote Displays' menu entry on the Frame.
# Usage: remote-displays/install.sh [install|uninstall]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
"$root/scripts/sync.sh" >/dev/null
apps=.local/share/applications
case ${1:-install} in
  install)
    fill_template "$root/remote-displays/ft-remote-displays.desktop" | on_frame "mkdir -p ~/$apps && cat > ~/$apps/ft-remote-displays.desktop"
    on_frame "chmod +x remote-displays/ft-remote-displays"
    echo "installed: Frametop Remote Displays" ;;
  uninstall)
    on_frame "rm -f ~/$apps/ft-remote-displays.desktop; echo removed" ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
