#!/usr/bin/env bash
# Install (or remove) the frame grabber our own eye tracker needs: ft-eyegrab, as the system
# service frametop-eyegrab.service. It copies the eye-camera frames, read-only, out of
# SteamVR's eyetracking process into /dev/shm/frametop-eyes-cams for ft-eyes, and only while
# ft-eyes wants them. The gaze service (gaze/ft-gazed) runs ft-eyes itself, when ours is the
# tracker in use (GAZE_TRACKER=auto, the default, picks it once this is installed) or the gaze
# probe uses it. install.sh offers this after gaze mode.
# Needs host sudo, for the binary (/etc/frametop/ft-eyegrab, root's), the unit, and the entry that
# keeps the binary through SteamOS updates (/etc/atomic-update.conf.d): it asks for
# the password in the terminal, on the Frame or from a PC, or runs SUDO_ASKPASS when that's set
# (frame_sudo in scripts/_env.sh, which also takes it from the repo's .env).
# Usage: gaze/tracker/install.sh [install|uninstall|status|log [lines]]
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
. "$root/scripts/_env.sh"
src=$FRAME_REPO/gaze/tracker
unit=frametop-eyegrab.service

sudo_run() { frame_sudo "$1"; }

case ${1:-install} in
  install)
    [ "$FRAME_RELEASE" = 1 ] || "$root/gaze/tracker/build.sh"
    ids=$(on_frame 'echo "$(id -u):$(id -g)"')
    fill_template "$root/gaze/tracker/$unit" | sed "s|@UID@|${ids%:*}|g; s|@GID@|${ids#*:}|g" |
      on_frame "cat > /tmp/$unit"
    sudo_run "set -e
install -D -m 0755 -o root -g root $src/build/ft-eyegrab /etc/frametop/ft-eyegrab
install -D -m 0644 -o root -g root /tmp/$unit /etc/systemd/system/$unit
install -D -m 0644 -o root -g root $src/atomic-update.conf /etc/atomic-update.conf.d/frametop-eyegrab.conf
rm -f /tmp/$unit
systemctl daemon-reload
systemctl enable $unit
systemctl restart $unit
sleep 1
echo \"$unit: \$(systemctl is-active $unit)\""
    ;;
  uninstall)
    sudo_run "systemctl disable --now $unit 2>/dev/null
rm -f /etc/systemd/system/$unit /etc/frametop/ft-eyegrab /etc/atomic-update.conf.d/frametop-eyegrab.conf
rmdir /etc/frametop 2>/dev/null; systemctl daemon-reload; echo removed" ;;
  status) on_frame "systemctl is-active $unit; ls -l /dev/shm/frametop-eyes-cams 2>/dev/null" || true ;;
  log) on_frame "journalctl -u $unit --no-pager -o cat -n ${2:-20}" ;;
  *) echo "usage: $0 [install|uninstall|status|log [lines]]" >&2; exit 2 ;;
esac
