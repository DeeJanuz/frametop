#!/usr/bin/env bash
# Install, start, stop, or inspect the gaze service (ft-gazed) on the Frame.
# Usage: gaze/run.sh install|uninstall   # user service, starts with SteamVR
#        gaze/run.sh start|stop|restart|status|log [lines]
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
unit=frametop-gaze.service
case ${1:-status} in
  install)
    [ "$FRAME_RELEASE" = 1 ] || "$root/gaze/build.sh"
    fill_template "$root/gaze/$unit" | on_frame "mkdir -p ~/.config/systemd/user && cat > ~/.config/systemd/user/$unit"
    on_frame "chmod +x gaze/ft-gazed gaze/ft-gazectl"
    "$frame" --host "set -e; systemctl --user daemon-reload; systemctl --user enable $unit
$(start_with_steamvr $unit)" ;;
  uninstall) "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; rm -f ~/.config/systemd/user/$unit; systemctl --user daemon-reload; echo removed" ;;
  start|stop|restart) "$frame" --host "systemctl --user $1 $unit; systemctl --user is-active $unit" ;;
  status) "$frame" --host "systemctl --user is-active $unit" || true; on_frame "gaze/ft-gazectl status" || true ;;
  log) "$frame" --host "journalctl --user -u $unit --no-pager -o cat -n ${2:-30}" ;;
  *) echo "usage: $0 install|uninstall|start|stop|restart|status|log" >&2; exit 2 ;;
esac
