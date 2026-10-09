#!/usr/bin/env bash
# Install, start, stop, or inspect hand tracking on the Frame: ft-camd (the camera broker) and
# ft-hands (the tracker), user services that stop with SteamVR. They don't start with it:
# ft-handsctl on|off (on the Frame) or hands/run.sh start|stop.
# Usage: hands/run.sh install|uninstall
#        hands/run.sh caps      # give ft-camd its capabilities again (a rebuild clears them)
#        hands/run.sh uncaps    # take them back, unless the Hand Recorder or the services use them
#        hands/run.sh start|stop|restart|status|log [lines]
# install and caps need the password (sudo setcap, once per build of ft-camd), and so do
# uninstall and uncaps when they take the capabilities back. It's asked in the terminal, on the
# Frame or from a PC (frame_sudo in scripts/_env.sh, which also takes it from the repo's .env).
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
units="frametop-camd.service frametop-hands.service"
# pidfd_getfd on XRService (ptrace_scope=1), system-wide tracepoints, and their root-only
# format files. ft-camd drops them all once it has set up.
caps=cap_sys_ptrace,cap_perfmon,cap_dac_read_search+ep
# Installed, two things use ft-camd's capabilities: the Hand Recorder (hands/rec/install.sh: its
# menu entry) and the services. ft-cutouts uses them too, but installs nothing.
handrec_entry='~/.local/share/applications/frametop-handrec.desktop'
camd_unit='~/.config/systemd/user/frametop-camd.service'

sudo_run() { frame_sudo "$1"; }

set_caps() {  # only when missing: a rebuild clears them, a reinstall doesn't
  local bin
  bin=$(printf %q "$FRAME_REPO/hands/build/ft-camd")
  if on_frame "getcap $bin | grep -q cap_sys_ptrace"; then
    echo "ft-camd has its capabilities"
    return
  fi
  sudo_run "setcap $caps $bin && getcap $bin"
}

# A left-over binary shouldn't keep its read-any-file powers, but while the Hand Recorder or
# the services are installed, they still need them. Best effort: without the password it warns.
drop_caps() {
  local bin who
  bin=$(printf %q "$FRAME_REPO/hands/build/ft-camd")
  who=$(on_frame "if ! { [ -x $bin ] && getcap $bin | grep -q cap_sys_ptrace; }; then echo none
elif [ -e $handrec_entry ]; then echo recorder
elif [ -e $camd_unit ]; then echo services
else echo nobody; fi") || who=none
  case $who in
    recorder) echo "kept ft-camd's capabilities: the Hand Recorder uses them" ;;
    services) echo "kept ft-camd's capabilities: hand tracking's services use them (hands/run.sh uninstall)" ;;
    nobody) sudo_run "setcap -r $bin" || echo "warning: couldn't drop ft-camd's capabilities (no password?)" >&2 ;;
  esac
}

states="for u in $units; do echo \"\$u: \$(systemctl --user is-active \$u)\"; done"

case ${1:-status} in
  install)
    "$root/hands/build.sh"
    set_caps
    for u in $units; do
      fill_template "$root/hands/$u" | on_frame "mkdir -p ~/.config/systemd/user && cat > ~/.config/systemd/user/$u"
    done
    # Installed but not started with SteamVR: ft-handsctl on|off (linked into ~/.local/bin).
    "$frame" --host "set -e; systemctl --user daemon-reload; systemctl --user disable $units 2>/dev/null || true
mkdir -p ~/.local/bin && ln -sfn $(printf %q "$FRAME_REPO/hands/ft-handsctl") ~/.local/bin/ft-handsctl
$states; echo 'start it with: ft-handsctl on'" ;;
  caps) set_caps ;;
  uncaps) drop_caps ;;
  uninstall)
    "$frame" --host "systemctl --user disable --now $units 2>/dev/null
for u in $units; do rm -f ~/.config/systemd/user/\$u; done; systemctl --user daemon-reload
[ -L ~/.local/bin/ft-handsctl ] && rm -f ~/.local/bin/ft-handsctl; echo removed"
    drop_caps ;;  # once the units are gone, so they don't count as a user
  start|stop|restart) "$frame" --host "systemctl --user $1 $units; $states" ;;
  status) "$frame" --host "$states; journalctl --user -u frametop-hands.service --no-pager -o cat -n 4" || true ;;
  log) "$frame" --host "journalctl --user -u frametop-camd.service -u frametop-hands.service --no-pager -o short -n ${2:-30}" ;;
  *) echo "usage: $0 install|uninstall|caps|uncaps|start|stop|restart|status|log [lines]" >&2; exit 2 ;;
esac
