#!/usr/bin/env bash
# Install (or remove) the persistent Bluetooth LE workarounds on the Frame.
# Needs host sudo: it asks for the password in the terminal, on the Frame or from a PC
# (frame_sudo in scripts/_env.sh, which also takes it from the repo's .env).
# Usage: setup/bluetooth/install.sh [install|uninstall|run]
#   run   re-apply now without restarting bluetooth (after pairing a new device)
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
src=$FRAME_REPO/setup/bluetooth

sudo_run() { frame_sudo "$1"; }

case ${1:-install} in
  install)
    "$root/scripts/sync.sh" >/dev/null
    sudo_run "set -e
install -D -m 0755 -o root -g root $src/bt-fixups.sh /etc/steamframe/bt-fixups.sh
install -D -m 0644 -o root -g root $src/steamframe-bt-fixups.service /etc/systemd/system/steamframe-bt-fixups.service
install -D -m 0644 -o root -g root $src/atomic-update.conf /etc/atomic-update.conf.d/frametop-bluetooth.conf
rm -f /etc/systemd/system/bluetooth.service.d/steamframe.conf
rmdir /etc/systemd/system/bluetooth.service.d 2>/dev/null || true
systemctl daemon-reload
systemctl enable steamframe-bt-fixups.service
echo installed" ;;
  uninstall)
    sudo_run "systemctl disable steamframe-bt-fixups.service 2>/dev/null
rm -f /etc/systemd/system/steamframe-bt-fixups.service /etc/systemd/system/bluetooth.service.d/steamframe.conf /etc/steamframe/bt-fixups.sh \
  /etc/atomic-update.conf.d/frametop-bluetooth.conf
rmdir /etc/systemd/system/bluetooth.service.d /etc/steamframe 2>/dev/null; systemctl daemon-reload; echo removed" ;;
  run) sudo_run "/etc/steamframe/bt-fixups.sh" ;;
  *) echo "usage: $0 [install|uninstall|run]" >&2; exit 2 ;;
esac
