# Sourced by the other scripts. Works out where the Steam Frame is and how to reach it.
#
# On the Frame itself (SteamOS, VR variant), FRAME_LOCAL=1: commands run locally, and the
# Frame's copy of the repo (FRAME_REPO) is this checkout, wherever it was cloned.
# On a PC, FRAME_LOCAL=0: commands run over SSH on $FRAME_HOST (default "frame"), and the
# Frame's copy is the one scripts/sync.sh keeps at ~/dev/frametop.
# FRAME_LOCAL, FRAME_HOST, FRAME_REPO, and FRAME_BOX (container, default "dev") can be
# set in the environment to override. FRAME_IN_BOX=1 means this already runs inside the
# build container (the image build, pack/Containerfile): commands run here, in this checkout.

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

[ "${FRAME_IN_BOX:-0}" = 1 ] && FRAME_LOCAL=1
if [ -z "${FRAME_LOCAL:-}" ]; then
  FRAME_LOCAL=0
  if grep -qx 'ID=steamos' /etc/os-release 2>/dev/null && grep -qE '^VARIANT_ID="?vr"?$' /etc/os-release; then
    FRAME_LOCAL=1
  fi
fi

if [ "$FRAME_LOCAL" = 1 ]; then
  FRAME_REPO=$REPO_ROOT
  # A terminal inside a Plasma session in VR (Frametop or the stock desktop) has that
  # session's private XDG_RUNTIME_DIR and D-Bus. systemctl --user and podman need the
  # real ones.
  export XDG_RUNTIME_DIR=/run/user/$(id -u)
  export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
else
  FRAME_REPO=${FRAME_REPO:-/home/steamos/dev/frametop}
  # Agent and IDE shells often inherit gpg-agent's socket while the keys are loaded
  # into keychain's ssh-agent at login. Use keychain's when the current one has no keys.
  if ! ssh-add -l >/dev/null 2>&1; then
    kc="$HOME/.keychain/$(hostname)-sh"
    # shellcheck disable=SC1090
    [ -f "$kc" ] && . "$kc" >/dev/null
  fi
fi
FRAME_HOST=${FRAME_HOST:-frame}
FRAME_BOX=${FRAME_BOX:-dev}

# on_frame '<command>': run a shell command on the Frame host, in FRAME_REPO.
on_frame() {
  if [ "$FRAME_LOCAL" = 1 ]; then
    (cd "$FRAME_REPO" && bash -c "$1")
  else
    ssh -o BatchMode=yes "$FRAME_HOST" "cd $(printf %q "$FRAME_REPO") && $1"
  fi
}

# on_frame_script [args...] < script: run a bash script from stdin on the Frame host.
on_frame_script() {
  if [ "$FRAME_LOCAL" = 1 ]; then
    bash -s -- "$@"
  else
    ssh -o BatchMode=yes "$FRAME_HOST" "bash -s -- $(printf '%q ' "$@")"
  fi
}

# fill_template <file>: print a file with @REPO@ replaced by the Frame's repo path.
fill_template() {
  sed "s|@REPO@|$FRAME_REPO|g" "$1"
}

# frame_sudo '<command>': run a shell command as root on the Frame host. sudo asks for the
# password in this terminal: on the Frame directly (it reads the terminal itself, so this
# works when stdin isn't one, as in install.sh's steps), or from a PC through ssh -t.
# SUDO_ASKPASS on the Frame, or steamos_root_pwd in the repo's .env (sent to sudo -S on
# stdin, never on a command line) answer it with no terminal; from a PC, .env comes first.
frame_sudo() {
  local tty=0 pw
  { : </dev/tty; } 2>/dev/null && tty=1
  if [ "$FRAME_LOCAL" = 1 ] && [ -n "${SUDO_ASKPASS:-}" ]; then
    sudo -A bash -c "$1"
    return
  fi
  if [ "$FRAME_LOCAL" = 1 ] && [ "$tty" = 1 ]; then
    sudo bash -c "$1"
    return
  fi
  pw=$(sed -n 's/^steamos_root_pwd=//p' "$REPO_ROOT/.env" 2>/dev/null)
  pw=${pw#[\"\']}; pw=${pw%[\"\']}  # .env values may be quoted
  if [ -n "$pw" ]; then
    printf '%s\n' "$pw" | on_frame "sudo -S -p '' bash -c $(printf %q "$1")"
  elif [ "$FRAME_LOCAL" = 0 ] && [ "$tty" = 1 ]; then
    ssh -tt -o BatchMode=yes "$FRAME_HOST" "cd $(printf %q "$FRAME_REPO") && sudo bash -c $(printf %q "$1")" </dev/tty
  else
    echo "sudo needs a terminal for the password, or steamos_root_pwd in $REPO_ROOT/.env" >&2
    return 1
  fi
}

# start_with_steamvr UNIT: a host command for an installer. Units that need SteamVR
# (Requisite=steamvr.service) can't start without it, so with SteamVR off (an install over
# SSH, the headset asleep) they're left to start with it. With SteamVR on, the unit restarts,
# so a re-install runs the new code, and the command waits for it and shows its last lines.
start_with_steamvr() {
  local unit=$1
  printf '%s' "if systemctl --user is-active --quiet steamvr.service; then
systemctl --user restart $unit
# distrobox enter takes a few seconds.
for i in \$(seq 20); do systemctl --user is-active --quiet $unit && break; sleep 1; done
echo \"$unit: \$(systemctl --user is-active $unit)\"; journalctl --user -u $unit --no-pager -o cat -n 3
else
echo '$unit: enabled; SteamVR is off, so it starts with SteamVR'
fi"
}
