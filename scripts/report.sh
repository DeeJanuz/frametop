#!/usr/bin/env bash
# Collect what a Frametop bug report needs into one text file: versions, service states,
# settings, and recent logs. Bluetooth addresses and the headset's serial number are masked.
# Usage: scripts/report.sh   (in a terminal on the Frame, or from a PC over SSH)
set -euo pipefail
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
. "$root/scripts/_env.sh"
out=$root/frametop-report-$(date +%Y%m%d-%H%M%S).txt

on_frame_script "$FRAME_REPO" > "$out" 2>&1 <<'EOF' || true
repo=$1
export XDG_RUNTIME_DIR=/run/user/$(id -u) DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/$(id -u)/bus
logs=~/.local/share/Steam/logs
section() { printf '\n===== %s\n' "$*"; }

section Versions
grep -E '^(PRETTY_NAME|VERSION_ID|BUILD_ID|VARIANT_ID)=' /etc/os-release
grep -m1 -o 'vrcompositor [0-9.]* startup' $logs/vrcompositor.txt 2>/dev/null || echo "SteamVR: not found"
git -C "$repo" log -1 --format='Frametop %h (%cd)' --date=short 2>/dev/null || echo "Frametop: not a git checkout"
~/.local/bin/distrobox version 2>/dev/null || echo "distrobox: not installed"
podman container inspect -f 'dev container: running={{.State.Running}} image={{.ImageName}}' dev 2>/dev/null || echo "dev container: missing"

section Services
for u in frametop-input-relay frametop-pointer frametop-power; do
  echo "$u: $(systemctl --user is-enabled $u 2>/dev/null) / $(systemctl --user is-active $u 2>/dev/null)"
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

section "Input relay (last 60 lines)"
journalctl --user -u frametop-input-relay -n 60 --no-pager -o short 2>/dev/null
section "Pointer helper (last 60 lines)"
journalctl --user -u frametop-pointer -n 60 --no-pager -o short 2>/dev/null
section "Power service (last 30 lines)"
journalctl --user -u frametop-power -n 30 --no-pager -o short 2>/dev/null
section "Gaze"
echo "frametop-gaze: $(systemctl --user is-enabled frametop-gaze 2>/dev/null) / $(systemctl --user is-active frametop-gaze 2>/dev/null)"
echo "frametop-eyegrab (our eye tracker's frame grabber): $(systemctl is-enabled frametop-eyegrab 2>/dev/null) / $(systemctl is-active frametop-eyegrab 2>/dev/null)"
for f in calibration.json eyes/calibration.json; do  # SteamVR's correction, our tracker's calibration
  p=~/.local/state/frametop/gaze/$f
  echo "$f: $([ -f "$p" ] && date -r "$p" '+%F %T' || echo none)"
done
python3 - "$repo" <<'PY' 2>&1 || true
import json, subprocess, sys
out = subprocess.run([sys.executable, sys.argv[1] + "/gaze/ft-gazectl", "status"], capture_output=True, text=True, timeout=10)
text = (out.stdout or out.stderr).strip()
try:
    print("status:", json.dumps(json.loads(text), separators=(",", ":")))
except ValueError:
    print("status:", text or "no answer from the gaze service")
PY
section "Gaze checks and calibration dots (last 10)"
tail -n 10 ~/.local/state/frametop/gaze/checks.jsonl 2>/dev/null | cut -c1-400 || true
section "Gaze service (last 60 lines, podman's left out)"
journalctl --user -u frametop-gaze -n 300 --no-pager -o short 2>/dev/null | grep -v ' podman\[' | tail -n 60
section "Our eye tracker's frame grabber (last 20 lines)"
journalctl -u frametop-eyegrab -n 20 --no-pager -o short 2>/dev/null
for f in /tmp/frametop-session.log /tmp/frametop-screens.log "$XDG_RUNTIME_DIR/frametop-layout.log"; do
  section "$f (last 60 lines)"
  tail -n 60 "$f" 2>/dev/null || echo "missing"
done
section "SteamVR server: Frametop and errors (last 60 lines)"
grep -a -i -E 'ft_pointer|frametop|\[error\]' $logs/vrserver.txt 2>/dev/null | tail -n 60
EOF

sed -i -E 's/([0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}/xx:xx:xx:xx:xx:xx/g; s/cv\.[A-Z0-9]{8,}/cv.<serial>/g' "$out"
echo "wrote $out"
echo "Attach it to an issue at https://github.com/DeeJanuz/frametop/issues, with what you did, what you expected, and what happened."
