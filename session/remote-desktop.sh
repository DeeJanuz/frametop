#!/bin/bash
# Runs on the Frame host. Internal capture server for remote access: KRdp's
# krdpserver (from the dev container) talks to the nested KWin directly
# (--plasma, no desktop portal) and serves it over RDP on 127.0.0.1 only.
# Nothing outside the Frame can reach it. vnc-bridge.sh connects to it and
# re-serves the desktop over VNC. Started by frametop-session.sh when REMOTE=1.
set -eu

runtime=${1:?usage: remote-desktop.sh <nested XDG_RUNTIME_DIR>}
# 3389 is taken by SteamOS's own xrdp (a separate X11 session, not the VR desktop).
port=${RDP_PORT:-3390}
creds=$HOME/.config/frametop-remote

mkdir -p -m 0700 "$creds"
# The password between krdp and vnc-bridge.sh (both on this host), new at every start: krdp
# takes it only on its command line, which other local users can read while it runs.
(umask 077; head -c 24 /dev/urandom | base64 | tr -d '/+=' | cut -c1-20 > "$creds/password")
if [ ! -s "$creds/cert.pem" ]; then
  (umask 077; openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=$(hostname)" \
    -keyout "$creds/key.pem" -out "$creds/cert.pem" 2>/dev/null)
fi

# Wait for the nested KWin to come up.
for _ in $(seq 60); do
  [ -S "$runtime/wayland-0" ] && break
  sleep 1
done

# podman needs the real runtime dir. The nested one is passed only to krdpserver.
export XDG_RUNTIME_DIR=/run/user/$(id -u)
exec "$(dirname "$(readlink -f "$0")")/../scripts/in-box" env XDG_RUNTIME_DIR="$runtime" WAYLAND_DISPLAY=wayland-0 QT_QPA_PLATFORM=wayland \
  krdpserver --plasma --address 127.0.0.1 --port "$port" \
  -u "$(id -un)" -p "$(cat "$creds/password")" \
  --certificate "$creds/cert.pem" --certificate-key "$creds/key.pem"
