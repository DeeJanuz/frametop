#!/usr/bin/env bash
# Start, stop, or inspect the multi-screen Plasma desktop in VR on the Frame.
# Usage: desktops.sh start [screens] | stop | restart | status | log [lines]
#        desktops.sh install     # make the VR launcher's "Desktop" entry start Frametop, and add
#                                # "Native Desktop" for the stock SteamOS desktop
#        desktops.sh uninstall   # give the launcher back the stock SteamOS desktop
#        desktops.sh screens N   # set the default screen count in ~/.config/frametop.conf
#        desktops.sh remote on|off|info  # VNC access over the tailnet (applies on next start)
#        desktops.sh relay install|uninstall|status|log  # input relay service (see input/input-relay.py)
# start without a count uses the Frame's config. FT_WIDTH, FT_HEIGHT, FT_PHYS_WIDTH pass through.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$root/scripts/_env.sh"
frame="$root/scripts/frame.sh"
action=${1:-start}
screens=${2:-${FT_SCREENS:-}}
session=$FRAME_REPO/session
override=.local/share/applications/deckard-nested-desktop.desktop
native_copy=.local/share/applications/native-deckard-nested-desktop.desktop
stock=/usr/share/applications/deckard-nested-desktop.desktop
log=/tmp/frametop-session.log
# Bracketed first letter so pgrep/pkill never match the ssh shell running them.
match='[v]r-overlay-key frametop '
# Is either backend's desktop running? (gamescope, or ft-screens)
running="{ pgrep -f '$match' >/dev/null || pgrep -x ft-screens >/dev/null; }"

case $action in
  start)
    # In its own systemd unit, so it outlives this shell (SSH, the settings app's restart).
    "$root/scripts/sync.sh" >/dev/null
    "$frame" --host "$running && { echo 'already running'; exit 0; }
systemctl --user reset-failed frametop-desktop 2>/dev/null
systemd-run --user --collect --quiet --unit frametop-desktop \
  ${screens:+--setenv=FT_SCREENS=$screens} ${FT_WIDTH:+--setenv=FT_WIDTH=$FT_WIDTH} ${FT_HEIGHT:+--setenv=FT_HEIGHT=$FT_HEIGHT} \
  ${FT_PHYS_WIDTH:+--setenv=FT_PHYS_WIDTH=$FT_PHYS_WIDTH} ${FT_BACKEND:+--setenv=FT_BACKEND=$FT_BACKEND} \
  bash -c 'exec $session/frametop-session.sh > $log 2>&1'
sleep 12; echo \"plasmashell processes: \$(pgrep -c plasmashell)\"
$running && echo 'started' || { echo 'failed:'; tail -20 $log; exit 1; }" ;;
  install)
    # Also "Native Desktop", a copy of SteamOS's entry for its own desktop. Optional, so a SteamOS
    # without the stock entry still installs. No X-Steam-Special, so Steam can only single out ours.
    "$root/scripts/sync.sh" >/dev/null
    "$frame" --host "set -e; mkdir -p ~/.local/share/applications
sed 's|@SESSION@|$session/frametop-session.sh|' $session/deckard-nested-desktop.desktop > ~/$override
if [ -r $stock ]; then
  sed -e 's/^Name=.*/Name=Native Desktop/' -e '/^Name\\[/d' -e '/^X-Steam-Special=/d' -e '/pick this out/d' \\
    $stock > ~/$native_copy.new && mv ~/$native_copy.new ~/$native_copy
else
  rm -f ~/$native_copy; echo 'no $stock: no Native Desktop entry' >&2
fi
[ -f ~/.config/frametop.conf ] || cp $session/frametop.conf.example ~/.config/frametop.conf
echo \"installed ~/$override\"; grep ^Exec= ~/$override; echo; cat ~/.config/frametop.conf" ;;
  uninstall)
    # Also what the session puts in place at each start: Launch as Standalone's app copies and
    # the title bar decoration (float/ft_apps.py, decoration/).
    "$frame" --host "rm -f ~/$override ~/$native_copy
rm -rf ~/.local/share/frametop/apps ~/.local/share/kwin/decorations/kwin4_decoration_qml_frametop
rmdir ~/.local/share/frametop 2>/dev/null; echo 'removed; the launcher uses the stock desktop again'" ;;
  screens)
    [[ ${2:-} =~ ^[1-9]$ ]] || { echo "usage: $0 screens N   (1-9)" >&2; exit 2; }
    "$frame" --host "set -e; f=~/.config/frametop.conf
[ -f \$f ] || cp $session/frametop.conf.example \$f
sed -i 's/^SCREENS=[0-9]*/SCREENS=$2/' \$f; grep ^SCREENS \$f" ;;
  remote)
    case ${2:-info} in
      on|off)
        v=$([ "$2" = on ] && echo 1 || echo 0)
        "$frame" --host "set -e; f=~/.config/frametop.conf
[ -f \$f ] || cp $session/frametop.conf.example \$f
grep -q '^REMOTE=' \$f || echo 'REMOTE=0           # 1 = serve the desktop over VNC on the tailnet (port 5900)' >> \$f
sed -i 's/^REMOTE=[01]/REMOTE=$v/' \$f; grep ^REMOTE \$f; echo 'applies the next time the desktop starts'" ;;
      info)
        "$frame" --host "grep ^REMOTE ~/.config/frametop.conf 2>/dev/null || echo 'REMOTE not set'
ip=\$(ip -4 -o addr show tailscale0 | awk '{print \$4}' | cut -d/ -f1)
echo \"VNC: \$(hostname):5900 on the tailnet (\$ip)  password \$(cat ~/.config/frametop-remote/vnc-password 2>/dev/null || echo '(created on first start)')\"
if pgrep -f '[X]vnc :20 ' >/dev/null; then echo 'vnc: running'; else echo 'vnc: not running'; fi
if pgrep -f '[k]rdpserver --plasma' >/dev/null; then echo 'capture (krdp, 127.0.0.1 only): running'; else echo 'capture (krdp): not running'; fi" ;;
      *) echo "usage: $0 remote on|off|info" >&2; exit 2 ;;
    esac ;;
  relay)
    unit=frametop-input-relay.service
    case ${2:-status} in
      install)
        "$root/scripts/sync.sh" >/dev/null
        # Enabled, not started: started under a running SteamVR it would grab the
        # mouse away from it. It comes up before SteamVR on the next start.
        fill_template "$root/input/$unit" | on_frame "mkdir -p ~/.config/systemd/user && cat > ~/.config/systemd/user/$unit"
        "$frame" --host "set -e
systemctl --user daemon-reload; systemctl --user enable $unit
echo 'enabled; starts before SteamVR on the next reboot or SteamVR restart'" ;;
      uninstall) "$frame" --host "systemctl --user disable --now $unit 2>/dev/null; systemctl --user clean --what=fdstore $unit 2>/dev/null; rm -f ~/.config/systemd/user/$unit; systemctl --user daemon-reload; echo removed" ;;
      status) "$frame" --host "systemctl --user is-enabled $unit 2>/dev/null; systemctl --user is-active $unit 2>/dev/null
echo \"fd store: \$(systemctl --user show -p NFileDescriptorStore --value $unit)\"
p=\$(pgrep -x vrserver | head -1); [ -n \"\$p\" ] && for e in \$(ls -l /proc/\$p/fd 2>/dev/null | grep -oE 'event[0-9]+( \\(deleted\\))?' | sort -u | tr ' ' '_'); do n=\${e%%_*}; echo \"vrserver has \$e: \$(cat /sys/class/input/\$n/device/name 2>/dev/null)\"; done; true" ;;
      log) "$frame" --host "journalctl --user -u $unit --no-pager -n ${3:-30}" ;;
      *) echo "usage: $0 relay install|uninstall|status|log" >&2; exit 2 ;;
    esac ;;
  stop)
    # ft-screens: ending it ends KWin and the session. gamescope can take a while to exit
    # on SIGTERM. Wait, then force it. First, programs started in the desktop move out of
    # its unit (session/keep-apps.sh), so background work in them outlives the restart.
    "$frame" --host "$running || { echo 'not running'; exit 0; }
$session/keep-apps.sh
systemctl --user stop frametop-desktop 2>/dev/null; pkill -x ft-screens; pkill -f '$match'
for i in \$(seq 20); do $running || { echo stopped; exit 0; }; sleep 0.5; done
pkill -KILL -x ft-screens
pkill -KILL -f '$match'; sleep 1
pkill -f '[m]ultidesk-session.sh --inner' 2>/dev/null; pkill -f '[k]rdpserver --plasma' 2>/dev/null
pkill -f '[X]vnc :20 ' 2>/dev/null; pkill -f '[x]freerdp /v:.*:3390' 2>/dev/null
$running && { echo 'still running'; exit 1; } || echo 'stopped (forced)'" ;;
  restart) "$0" stop; sleep 3; exec "$0" start ${screens:+"$screens"} ;;
  status) "$frame" --host "if $running; then pgrep -af '$match|[m]d-screens --socket' | cut -c1-120; else echo 'not running'; fi" ;;
  log) "$frame" --host "grep -vE '^\s*$' $log | tail -n ${2:-40}" ;;
  *) echo "usage: $0 start [screens] | stop | restart | status | log [lines] | install | uninstall | screens N | remote on|off|info | relay install|uninstall|status|log" >&2; exit 2 ;;
esac
