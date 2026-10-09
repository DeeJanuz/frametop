#!/usr/bin/env bash
# Install the hand recorder on a Frame that has Frametop (get.sh --experimental): bring the dev
# container's packages up to date, build hand tracking's camera broker and tracker (hands/build.sh:
# the first build fetches and builds ncnn, a few minutes) and the headset panel (hands/rec/build.sh),
# give ft-camd its capabilities (hands/run.sh caps: asks for the password, once per build), and add
# "Frametop Hand Recorder" to the app menu (for Frametop's desktop: it isn't tested from SteamVR's
# "Launch a program", which runs apps outside it; the standalone recorder is for that).
# It doesn't turn on Frametop's live hand tracking (that's hands/run.sh install, still deferred).
# Usage: hands/rec/install.sh            install or update
#        hands/rec/install.sh uninstall  remove the menu entry, and ft-camd's capabilities unless
#                                        hand tracking's services use them (hands/run.sh uncaps).
#                                        Recordings stay where they are.
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
entry='~/.local/share/applications/frametop-handrec.desktop'

case ${1:-install} in
  install)
    echo "== 1/4 dev container packages"
    "$root/setup/dev-container.sh"
    echo "== 2/4 hand tracking: ft-camd and ft-hands"
    "$root/hands/build.sh"
    echo "== 3/4 the headset panel: ft-handpanel"
    "$root/hands/rec/build.sh"
    echo "== 4/4 ft-camd's capabilities (asks for your password) and the menu entry"
    "$root/hands/run.sh" caps
    "$root/scripts/conf-migrate.sh"   # HANDS_SWAP_SIDES=0, the old default, becomes auto
    fill_template "$root/hands/rec/ft-handrec.desktop" |
      on_frame "mkdir -p ~/.local/share/applications && cat > $entry"
    echo
    echo "Installed. On Frametop's desktop, open \"Frametop Hand Recorder\" from the app menu."
    echo "Recordings go to ~/.local/share/frametop/hands/contrib."
    ;;
  uninstall)
    on_frame "rm -f $entry"
    echo "Removed the menu entry."
    "$root/hands/run.sh" uncaps  # after the entry, which counts as a user of the capabilities
    echo "Your recordings are still in ~/.local/share/frametop/hands/contrib:"
    echo "delete that folder to remove them."
    ;;
  *) echo "usage: $0 [install|uninstall]" >&2; exit 2 ;;
esac
