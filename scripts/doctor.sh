#!/usr/bin/env bash
# Check that the Steam Frame is ready for these projects: reachable (from a PC), the
# distrobox tool and the dev container present, and disk space. Then check what Frametop
# needs from SteamOS, which an update can change (scripts/update-check.py). Works on the
# Frame too.
# Usage: scripts/doctor.sh [--mark-good]
#   --mark-good  once Frametop works, record the SteamOS, SteamVR, and KWin versions, so a
#                later run says what an update changed
set -uo pipefail

. "$(dirname "${BASH_SOURCE[0]}")/_env.sh"

fail=0
check() {
  local label=$1; shift
  if out=$("$@" 2>&1); then
    printf 'ok    %s%s\n' "$label" "${out:+: $out}"
  else
    printf 'FAIL  %s%s\n' "$label" "${out:+: $out}"
    fail=1
    return 1
  fi
}

# Like on_frame, but in the home folder: these checks don't need the repo, which a PC only
# copies to the Frame at its first sync (./install.sh does that).
on_home() {
  if [ "$FRAME_LOCAL" = 1 ]; then
    (cd ~ && bash -c "$1")
  else
    ssh -o BatchMode=yes "$FRAME_HOST" "$1"
  fi
}

if [ "$FRAME_LOCAL" = 1 ]; then
  echo "running on the Frame: $FRAME_REPO"
else
  check "ssh key loaded" bash -c 'ssh-add -l | grep -c . | sed "s/$/ key(s)/"'
  check "ssh to $FRAME_HOST" ssh -o BatchMode=yes -o ConnectTimeout=8 "$FRAME_HOST" true
  if [ "$fail" = 1 ]; then
    echo "Skipping device checks. Is the headset on and awake, and on the network?"
    exit 1
  fi
fi
check "SteamOS" on_home '. /etc/os-release; echo "$PRETTY_NAME $VERSION_ID build $BUILD_ID, $(uname -m)"'
check "distrobox" on_home 'test -x ~/.local/bin/distrobox && ~/.local/bin/distrobox version | head -1'
check "container $FRAME_BOX" on_home "podman ps -a --filter name=^$FRAME_BOX\$ --format '{{.Image}} {{.Status}}' | grep ."
check "free space in ~" on_home "df -h ~ | awk 'NR==2{print \$4\" free\"}'"
if check "repo on the Frame" on_frame 'pwd'; then
  # A taskbar saved on a screen the desktop doesn't have is hidden; the desktop's next start
  # moves it to the first screen (session/fix-panels.py).
  check "taskbar on a screen" on_frame 'set -o pipefail; [ -f session/fix-panels.py ] || { echo "not checked (older checkout)"; exit 0; }
  python3 session/fix-panels.py --check | paste -sd ";" | sed "s/;/; /g"'
else
  echo "      ./install.sh (or scripts/sync.sh) copies the repo there; the taskbar check needs it"
fi

echo "what Frametop needs from SteamOS:"
if [ "$FRAME_LOCAL" = 1 ]; then
  python3 "$REPO_ROOT/scripts/update-check.py" "$@" || fail=1
else
  ssh -o BatchMode=yes "$FRAME_HOST" "python3 - ${*:+$(printf '%q ' "$@")}" < "$REPO_ROOT/scripts/update-check.py" || fail=1
fi
exit "$fail"
