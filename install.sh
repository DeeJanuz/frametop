#!/usr/bin/env bash
# Install everything on the Steam Frame: the build container, Frametop (multi-screen
# desktop, input relay, universal 3D mouse, settings app), and optionally gaze mode and the
# Bluetooth fixes. Run it on the headset in a terminal, from this repo. It's safe to re-run,
# for example after `git pull`. (Hand tracking, hands/, is deferred: it isn't offered here.)
# (It also works from a PC over SSH; see "Developing from a PC" in the README.)
#
# Usage: ./install.sh [--yes] [--no-bluetooth]
#   --yes           don't ask; installs gaze mode, skips the Bluetooth fixes and the SteamVR
#                   restart
#   --no-bluetooth  don't offer the Bluetooth fixes
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$root/scripts/_env.sh"

assume_yes=0 bluetooth=1
for arg in "$@"; do
  case $arg in
    --yes) assume_yes=1 ;;
    --no-bluetooth) bluetooth=0 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown option: $arg" >&2; exit 2 ;;
  esac
done

# Only the questions read from the terminal (or whatever stdin is); the build steps get
# no input, so they can't swallow typed-ahead or piped answers.
exec 3<&0 </dev/null

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
ask() {  # ask "question" default(y|n)
  [ "$assume_yes" = 1 ] && [ "$2" = y ] && return 0
  [ "$assume_yes" = 1 ] && return 1
  local hint answer
  hint=$([ "$2" = y ] && echo "Y/n" || echo "y/N")
  read -r -p "$1 [$hint] " answer <&3 || answer=
  answer=${answer:-$2}
  [[ $answer =~ ^[Yy] ]]
}

if [ "$FRAME_LOCAL" = 1 ]; then
  echo "Installing on this Steam Frame from $FRAME_REPO"
else
  echo "Installing on $FRAME_HOST over SSH (repo copy at $FRAME_REPO)"
  "$root/scripts/sync.sh" >/dev/null
fi
if [ "$FRAME_LOCAL" = 0 ] || [ -n "${SSH_CONNECTION:-}" ]; then
  echo "Over SSH: if the connection drops, run this again (it keeps what it downloaded). Services"
  echo "that need SteamVR start with it if it isn't running now."
fi

step "1/9 distrobox (container tool, installed in your home folder)"
if on_frame 'test -x ~/.local/bin/distrobox'; then
  echo "already installed: $(on_frame '~/.local/bin/distrobox version | head -1')"
else
  on_frame 'set -e; mkdir -p ~/dev/src
# A tested release, so upstream changes cannot break new installs.
[ -d ~/dev/src/distrobox ] || git clone --depth 1 --branch 1.8.2.5 https://github.com/89luca89/distrobox.git ~/dev/src/distrobox
cd ~/dev/src/distrobox && ./install --prefix ~/.local'
fi

step "2/9 build container (Fedora 44 'dev', about 1-2 GB the first time)"
"$root/setup/dev-container.sh"

step "3/9 input relay (keeps Bluetooth mice working in SteamVR, device roles, button maps)"
"$root/desktops.sh" relay install

step "4/9 3D mouse: SteamVR driver"
"$root/pointer/driver/build.sh"
"$root/pointer/driver/install.sh" install 2>&1 | grep -v xdg-open

step "5/9 3D mouse: pointer helper service"
"$root/pointer/helper/build.sh"
"$root/pointer/helper/run.sh" install

step "6/9 power service (turns the displays off while the headset isn't used, even on a stand)"
"$root/power/build.sh"
"$root/power/run.sh" install

step "7/9 multi-screen desktop (ft-screens), Frametop Input Settings, and Frametop Display Settings"
"$root/screens/build.sh"
"$root/desktops.sh" install >/dev/null
"$root/input-settings/install.sh"
"$root/display-settings/install.sh"
"$root/remote/install.sh"
on_frame "sed -i 's/^POINTER=0/POINTER=1/' ~/.config/frametop.conf; grep -q '^POINTER=' ~/.config/frametop.conf || echo 'POINTER=1' >> ~/.config/frametop.conf"
echo "the launcher's Desktop entry now opens the multi-screen desktop; 3D mouse on (POINTER=1 in ~/.config/frametop.conf)"
share=n
on_frame "grep -q '^SHARE_CONFIG=1' ~/.config/frametop.conf" && share=y
if ask "Let apps in the Frametop desktop use your normal config (shell, editor, logins)? Plasma's own settings stay separate." $share; then
  share=1
else
  share=0
fi
on_frame "sed -i 's/^SHARE_CONFIG=[0-9]*/SHARE_CONFIG=$share/' ~/.config/frametop.conf; grep -q '^SHARE_CONFIG=' ~/.config/frametop.conf || echo 'SHARE_CONFIG=$share' >> ~/.config/frametop.conf"
echo "SHARE_CONFIG=$share in ~/.config/frametop.conf (takes effect at the next desktop start)"

step "8/9 gaze mode (optional, experimental: the pointer goes where you look)"
if ask "Install gaze mode? You turn it on and calibrate it in Frametop Input Settings, on the Gaze page." y; then
  "$root/gaze/run.sh" install
else
  echo "skipped. Install later with: gaze/run.sh install"
fi

step "9/9 Bluetooth fixes (optional; they let LE mice and keyboards like the Swiftpoint Z3 reconnect)"
if [ "$bluetooth" = 1 ] && ask "Install the Bluetooth fixes? They need your password (sudo)." n; then
  "$root/setup/bluetooth/install.sh" install
else
  echo "skipped. Install later with: setup/bluetooth/install.sh install"
fi

step "Done"
cat <<'EOF'
SteamVR has to restart once, to load the 3D mouse driver and to start the input relay
before it. Restarting SteamVR closes everything open in VR, including this terminal if
it's in a VR desktop. Rebooting the headset works too.

Recommended: in Frametop Display Settings > Power, choose when the displays turn off
while the headset isn't used (for a stand or mount that covers its proximity sensor), and
turn on Stay awake while plugged in, so Steam doesn't put the headset to sleep while it
charges. The displays still turn off when you take the headset off.
EOF
if ask "Restart SteamVR now?" n; then
  on_frame 'systemctl --user restart steamvr.service'
fi
