#!/usr/bin/env bash
# Collect what a Frametop bug report needs into one text file: versions, service states,
# settings, Frametop's keyboard and the Steam menu, recent logs, and the gaze report
# (scripts/gaze-report.py). Bluetooth addresses and the headset's serial number are masked.
# Usage: scripts/report.sh [--watch [SECONDS]]   (in a terminal on the Frame, or from a PC over SSH)
#   --watch   then record SECONDS (default 60) while you make the problem happen: the
#             desktop's state (the Steam menu, our keyboard, which laser drags what, where
#             typing goes) whenever it changes, and the logs from that time
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
watch=0
case ${1:-} in
  "") ;;
  --watch) watch=${2:-60}
           [[ $watch =~ ^[0-9]+$ ]] && [ "$watch" -ge 5 ] && [ "$watch" -le 600 ] ||
             { echo "--watch takes 5 to 600 seconds" >&2; exit 2; } ;;
  -h|--help) sed -n '2,8p' "$0"; exit 0 ;;
  *) echo "usage: $0 [--watch [SECONDS]]" >&2; exit 2 ;;
esac
out=$root/frametop-report-$(date +%Y%m%d-%H%M%S).txt
echo "Collecting (up to half a minute: the gaze service is woken to see whether the eye tracker sends)..."

on_frame_script "$FRAME_REPO" > "$out" 2>&1 <<'EOF' || true
repo=$1
export XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
logs=~/.local/share/Steam/logs
section() { printf '\n===== %s\n' "$*"; }
# ask SOCKET COMMAND: a request to one of Frametop's abstract datagram sockets; prints the reply.
ask() {
  python3 -I - "$1" "$2" <<'PY' 2>&1
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind("")
s.settimeout(2)
try:
    s.sendto(sys.argv[2].encode(), "\0" + sys.argv[1])
    print(s.recv(65536).decode())
except OSError as e:
    print(f"no answer from @{sys.argv[1]} ({e})")
PY
}

section Versions
grep -E '^(PRETTY_NAME|VERSION_ID|BUILD_ID|VARIANT_ID)=' /etc/os-release
# The log keeps earlier runs: the last start is this SteamVR.
grep -a -o 'vrcompositor [0-9.]* startup' $logs/vrcompositor.txt 2>/dev/null | tail -n 1 | grep . || echo "SteamVR: not found"
if [ -f "$repo/.frametop-release" ]; then
  rel() { sed -n "s/^$1=//p" "$repo/.frametop-release" | tail -1; }
  echo "Frametop release $(rel VERSION) ($(rel COMMIT | cut -c1-7), $(rel CHANNEL)) in $repo"
  box=$(rel BOX)
else
  git -C "$repo" log -1 --format='Frametop %h (%cd)' --date=short 2>/dev/null || echo "Frametop: not a git checkout"
  git -C "$repo" branch --show-current 2>/dev/null | sed 's/^/branch: /'
  box=${FRAME_BOX:-dev}
fi
~/.local/bin/distrobox version 2>/dev/null || echo "distrobox: not installed"
podman container inspect -f "container $box: running={{.State.Running}} image={{.ImageName}}" "$box" 2>/dev/null ||
  echo "container $box: missing"

section Services
for u in frametop-input-relay frametop-pointer frametop-power frametop-gaze frametop-desktop; do
  echo "$u: $(systemctl --user is-enabled $u 2>/dev/null) / $(systemctl --user is-active $u 2>/dev/null)"
done
for u in frametop-eyegrab steamframe-bt-fixups; do
  echo "$u (system): $(systemctl is-enabled $u 2>/dev/null) / $(systemctl is-active $u 2>/dev/null)"
done
echo "desktop: $(pgrep -x ft-screens >/dev/null && echo running || echo 'not running'), plasmashell: $(pgrep -c plasmashell || true)"
echo "paused for VR games: $(cat /run/user/$(id -u)/frametop-pause.json 2>/dev/null || echo 'no (never paused since boot)')"
# SteamVR's config folder: a terminal in the desktop has the session's own (docs/design.md).
XDG_CONFIG_HOME=$HOME/.config LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrpathreg show 2>/dev/null | sed -n '/xternal/,$p'

section "What Frametop needs from SteamOS (scripts/update-check.py)"
python3 "$repo/scripts/update-check.py" 2>&1 || true

section Settings
grep -v '^\s*#' ~/.config/frametop.conf 2>/dev/null | sed 's/\s*#.*//' | grep . || echo "no ~/.config/frametop.conf"
python3 - <<'PY' 2>/dev/null || echo "no ~/.config/frametop-layout.json"
import json, os
d = json.load(open(os.path.expanduser("~/.config/frametop-layout.json")))
print("mode:", d.get("mode"), " auto:", d.get("auto"), " primary:", d.get("primary"))
for i, s in enumerate(d.get("screens", []), 1):
    print(f"screen {i}: {s.get('size')} {s.get('metres')} m curve={s.get('curve', 0)} pinned={s.get('pin', {}).get('hand', '-') if isinstance(s.get('pin'), dict) else '-'}")
print("visibility:", d.get("visibility"))
PY

section "Plasma panels and outputs"
python3 "$repo/session/fix-panels.py" --check 2>&1 || true
python3 - "$repo" <<'PY' 2>&1 || true
import json, subprocess, sys
sys.path.insert(0, sys.argv[1] + "/layout")
import ft_layout
env = ft_layout.nested_env()
if not env:
    sys.exit(print("desktop not running: no live outputs or panels"))
outs = json.loads(subprocess.run(["kscreen-doctor", "-j"], capture_output=True, text=True, env=env, timeout=10).stdout or "{}")
for o in outs.get("outputs", []):
    size = o.get("size") or {}
    print(f"output {o.get('name')}: enabled={o.get('enabled')} priority={o.get('priority')} {size.get('width')}x{size.get('height')}")
js = "print(JSON.stringify(panels().map(p => ({id: p.id, screen: p.screen, location: p.location}))))"
r = subprocess.run(["qdbus6", "org.kde.plasmashell", "/PlasmaShell", "org.kde.PlasmaShell.evaluateScript", js],
                   capture_output=True, text=True, env=env, timeout=10)
print("live panels:", (r.stdout or r.stderr).strip())
PY

section "Frametop's keyboard and the Steam menu"
python3 - "$(ask frametop_relay devices)" <<'PY' 2>&1
import json, os, sys
try:
    rules = json.load(open(os.path.expanduser("~/.config/frametop-input.json")))
except (OSError, ValueError):
    rules = {}
print("Keyboard setting (Input Settings > Keyboard):", rules.get("vr_keyboard", "no_keyboard (the default)"),
      "| keep it open:", rules.get("vr_keyboard_persist", True))
print("the relay's devices (with the default setting, a pass-through keyboard keeps ours closed):")
try:
    d = json.loads(sys.argv[1])
except ValueError:
    sys.exit(print("  " + sys.argv[1].strip()))
print(f"  pointer mode: {d.get('pointer_mode')}, paused: {d.get('paused')}")
for n in d.get("nodes", []):
    print("  " + json.dumps(n))
PY
echo "ft-textinput (tells the relay about text fields): $(pgrep -f '[f]t-textinput' >/dev/null && echo running || echo 'not running')"
echo "the desktop's KWin input method: $(pgrep -a kwin_wayland | grep -o -- '--inputmethod [^ ]*' | head -n 1 | grep . || echo 'none (the keyboard never opens for text fields)')"
for p in $(pgrep -x plasmashell); do
  env=$(tr '\0' '\n' < /proc/$p/environ 2>/dev/null) || continue
  echo "$env" | grep -q '^XDG_CONFIG_HOME=.*/frametop$' || continue
  for v in QT_IM_MODULE GTK_IM_MODULE; do  # xim here: Qt and GTK apps never ask for the keyboard
    printf "the desktop's %s: %s\n" $v "$(echo "$env" | sed -n "s/^$v=//p" | grep . || echo unset)"
  done
done
echo "ft-screens debug: $(ask ft_screens debug)"
echo "ft-screens state (mode, manual, wrist, gesture hand and angle, lasers, game, in games): $(ask ft_screens state)"
echo "ft-screens pause: $(ask ft_screens 'pause state'), concealed: $(ask ft_screens concealed)"
echo "SteamVR overlays shown now:"
XDG_CONFIG_HOME=$HOME/.config LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 timeout 10 \
  /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null |
  awk '/^\x27/ { show = $0 !~ /not_visible/ } show' | sed 's/^/  /' | head -n 80
echo "keyboard, drag and Steam menu lines from ft-screens (last 40):"
grep -a -E 'keyboard|Steam (in front|out of the way)|: (move|resize|roll)( by| ended| not started)' \
  /tmp/frametop-screens.log 2>/dev/null | tail -n 40 | sed 's/^/  /'
echo "text fields, from the relay (last 20):"
journalctl --user -u frametop-input-relay --no-pager -o short 2>/dev/null | grep 'text field' | tail -n 20 | sed 's/^/  /'

section "Input relay (last 60 lines)"
journalctl --user -u frametop-input-relay -n 60 --no-pager -o short 2>/dev/null
section "Pointer helper (last 60 lines)"
journalctl --user -u frametop-pointer -n 60 --no-pager -o short 2>/dev/null
section "Power service (last 30 lines)"
journalctl --user -u frametop-power -n 30 --no-pager -o short 2>/dev/null
# The gaze service, SteamVR's eye tracker and ours, the checks and calibrations, with what
# looks wrong first. It wakes an idle gaze service for about 20 s, to see the tracker send.
python3 "$repo/scripts/gaze-report.py" </dev/null 2>&1 || echo "scripts/gaze-report.py failed"
for f in /tmp/frametop-session.log /tmp/frametop-screens.log "$XDG_RUNTIME_DIR/frametop-layout.log"; do
  section "$f (last 60 lines)"
  tail -n 60 "$f" 2>/dev/null || echo "missing"
done
section "SteamVR server: Frametop and errors (last 60 lines)"
grep -a -i -E 'ft_pointer|frametop|\[error\]' $logs/vrserver.txt 2>/dev/null | tail -n 60
EOF

if [ "$watch" -gt 0 ]; then
  echo
  echo "Now make the problem happen within $watch seconds: for example, open the Steam menu, try to"
  echo "drag a window by its bar, and click into a text field. Recording..."
  on_frame_script "$watch" >> "$out" 2>&1 <<'EOF' || true
seconds=$1
printf '\n===== Watched for %s s from %s\n' "$seconds" "$(date '+%F %T')"
since=$(date '+%F %T')
start=$(wc -l < /tmp/frametop-screens.log 2>/dev/null || echo 0)
python3 -I - "$seconds" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
s.bind("")
s.settimeout(1)
end, last = time.monotonic() + float(sys.argv[1]), None
print("ft-screens' debug line whenever it changed:")
while time.monotonic() < end:
    try:
        s.sendto(b"debug", "\0ft_screens")
        line = s.recv(4096).decode()
    except OSError as e:
        line = f"no answer ({e})"
    if line != last:
        print(time.strftime("%H:%M:%S") + f"{time.time() % 1:.2f}"[1:], line, flush=True)
        last = line
    time.sleep(0.2)
PY
printf '\n===== ft-screens log while watching\n'
tail -n +"$((start + 1))" /tmp/frametop-screens.log 2>/dev/null | tail -n 300
for u in frametop-input-relay frametop-pointer; do
  printf '\n===== %s while watching\n' "$u"
  journalctl --user -u $u --since "$since" --no-pager -o short 2>/dev/null | tail -n 200
done
EOF
  echo "Recorded."
fi

sed -i -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g; s/cv\.[A-Z0-9]{8,}/cv.<serial>/g' "$out"
echo "wrote $out"
echo "Attach it to an issue at https://github.com/Frametop/frametop/issues, with what you did, what you expected, and what happened."
