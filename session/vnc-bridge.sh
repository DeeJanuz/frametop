#!/bin/bash
# Runs on the Frame host. Serves the Frametop desktop's primary screen (the one with the
# taskbar) over VNC for clients like RealVNC Viewer or macOS Screen Sharing. No VNC server
# here can capture KWin directly, so this bridges through krdp: Xvnc (a virtual X screen
# served over VNC) runs a FreeRDP client connected to krdpserver on 127.0.0.1. Both run in
# the dev container. VNC listens on the tailnet address only.
# Started by frametop-session.sh when REMOTE=1, after remote-desktop.sh.
#
# krdp streams the whole workspace (every screen). Its --monitor would stream one screen,
# but krdp 6.7 then maps the pointer as if that screen sat at 0,0, so clicks miss on a
# screen placed lower or further right. Instead the VNC screen is the primary's size, and
# the workspace-sized RDP window inside it is shifted so the primary fills it. The pointer
# maps 1:1. When the layout changes, the VNC screen resizes and the RDP client reconnects.
#
# The RDP client runs only while someone watches. While connected, krdp captures and
# H.264-encodes every redraw in software, about 60% of a core, with FreeRDP and Xvnc adding
# about 30% more, even with no VNC viewer. krdp starts its capture per RDP connection and
# stops it when the connection closes, so it idles without one. So FreeRDP starts when a
# VNC client connects (the screen is black for the few seconds that takes) and stops
# VNC_IDLE_SEC (45) seconds after the last one leaves. Xvnc has no hook for clients, so ss
# counts them: whenever Xvnc writes to its log (it logs each connection), every second
# while FreeRDP runs, and every 5 seconds otherwise. The log only wakes this script up;
# what it says doesn't matter.
set -eu

here=$(dirname "$(readlink -f "$0")")
vnc_port=${VNC_PORT:-5900}
rdp_port=${RDP_PORT:-3390}
display=:20
creds=$HOME/.config/frametop-remote

addr=$(ip -4 -o addr show tailscale0 2>/dev/null | awk '{print $4}' | cut -d/ -f1)
if [ -z "$addr" ]; then
  echo "tailscale0 has no address, not starting VNC" >&2
  exit 1
fi

# The VNC password is limited to 8 characters by the protocol. The traffic is
# still encrypted by the tailnet (WireGuard).
mkdir -m 0700 -p "$creds"  # remote-desktop.sh normally creates it; we may get here first
if [ ! -s "$creds/vnc-password" ]; then
  (umask 077; head -c 12 /dev/urandom | base64 | tr -d '/+=' | cut -c1-8 > "$creds/vnc-password")
fi

# Wait for krdpserver (started by remote-desktop.sh).
for _ in $(seq 60); do
  ss -ltn | grep -q "127.0.0.1:$rdp_port " && break
  sleep 1
done

# "x y width height workspace_width workspace_height" of the primary screen, once Plasma is up.
view() { "$here/../layout/ft-layout" remote-view 2>/dev/null | grep -xE '[0-9]+( [0-9]+){5}'; }
v=
for _ in $(seq 90); do
  v=$(view) && [ -n "$v" ] && break
  v=
  sleep 1
done
if [ -z "$v" ]; then
  echo "couldn't read the desktop's screens, not starting VNC" >&2
  exit 1
fi
read -r _ _ w h _ _ <<< "$v"

export XDG_RUNTIME_DIR=/run/user/$(id -u)
box() { "$here/../scripts/in-box" "$@"; }
stop_rdp() { pkill -f "[x]freerdp /v:127.0.0.1:$rdp_port " 2>/dev/null || true; }
trap 'stop_rdp; pkill -f "[X]vnc $display " 2>/dev/null || true' EXIT

box bash -c 'vncpasswd -f < "$1/vnc-password" > "$1/vnc-passwd.bin" && chmod 600 "$1/vnc-passwd.bin"' - "$creds"
exec {xlog}< <(box Xvnc "$display" -geometry "${w}x${h}" -depth 24 \
  -interface "$addr" -rfbport "$vnc_port" \
  -SecurityTypes VncAuth -PasswordFile "$creds/vnc-passwd.bin" \
  -AlwaysShared -desktop "Steam Frame (Frametop)" 2>&1)
xvnc=$!

# Wait up to $1 seconds, less if Xvnc logs something; its lines go on to this log.
nap() {
  local line rc=0
  IFS= read -rt "$1" -u "$xlog" line || rc=$?
  if [ $rc -eq 0 ]; then
    printf '%s\n' "$line"
    while IFS= read -rt 0.1 -u "$xlog" line; do printf '%s\n' "$line"; done
  elif [ $rc -le 128 ]; then
    sleep 1  # Xvnc's output closed: it's exiting
  fi
  return 0
}
nap 2

# A connected VNC client: an established TCP connection to Xvnc's port. Any connection
# counts, authenticated or not; it's on the tailnet only.
clients() { [ -n "$(ss -Htn state established "( sport = :$vnc_port )" 2>/dev/null)" ]; }

# ft-screens drops screens you aren't looking at to a low frame rate, and krdp would
# stream that. "watch SECONDS" asks it for full rate on every screen for that long: sent
# when a client connects and renewed every few seconds while one stays, so it lapses by
# itself if this script dies. An older ft-screens just answers that it doesn't know it.
watch() {
  if command -v socat >/dev/null; then
    printf 'watch 15' | socat -u - ABSTRACT-SENDTO:ft_screens 2>/dev/null
  else
    python3 -c 'import socket; socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM).sendto(b"watch 15", "\0ft_screens")' 2>/dev/null
  fi || true
}

# What remote-view depends on: KWin's saved outputs (positions, scales, primary) and
# Frametop's layout (screen sizes). It's read again only after one of these changes, and
# once a minute in case a change didn't touch them, and only while FreeRDP runs.
layout_files=("$HOME/.config/frametop/kwinoutputconfig.json" "$HOME/.config/frametop-layout.json")
stamp() { stat -c %y "${layout_files[@]}" 2>/dev/null || true; }

# Start FreeRDP inside the VNC screen, sized and shifted for $v.
# /cert:ignore is fine here: the connection never leaves this host.
start_rdp() {
  read -r x y w h ww wh <<< "$v"
  box env DISPLAY=$display bash -c '
    size=$1 x=$2 y=$3 ww=$4 wh=$5 creds=$6 rdp_port=$7
    if [ "$(xrandr | sed -n "s/.*current \([0-9]*\) x \([0-9]*\),.*/\1x\2/p")" != "$size" ]; then
      xrandr --newmode "$size" 0 "${size%x*}" 0 0 0 "${size#*x}" 0 0 0 2>/dev/null || true
      xrandr --addmode VNC-0 "$size" 2>/dev/null || true
      xrandr --fb "$size" --output VNC-0 --mode "$size"
    fi
    xfreerdp /v:127.0.0.1:"$rdp_port" /u:"$(id -un)" /p:"$(cat "$creds/password")" \
      /cert:ignore /size:"${ww}x${wh}" -decorations +clipboard >/dev/null 2>&1 &
    rdp=$!
    # FreeRDP takes no negative position, so move its window once it is up.
    for _ in $(seq 60); do
      kill -0 $rdp 2>/dev/null || break
      win=$(xdotool search --class xfreerdp 2>/dev/null | tail -1)
      [ -n "$win" ] && break
      sleep 0.5
    done
    [ -n "$win" ] && xdotool windowmove "$win" "$((-x))" "$((-y))"
    wait $rdp
  ' vnc-rdp "${w}x$h" "$x" "$y" "$ww" "$wh" "$creds" "$rdp_port" || true &
  rdp=$!
}

idle_sec=${VNC_IDLE_SEC:-45}
rdp=           # FreeRDP's job while it runs
seen=0         # when a client was last seen ($SECONDS)
watched=-99    # when "watch" was last sent
stamp_v=       # stamp() when $v was read
recheck=0      # read the layout every 2 seconds until then
next_view=0    # next time to read it
while kill -0 $xvnc 2>/dev/null; do
  if clients; then
    if [ $((SECONDS - watched)) -ge 5 ]; then watch; watched=$SECONDS; fi
    seen=$SECONDS
    if [ -z "$rdp" ]; then
      echo "VNC client connected, starting the RDP client"
      now=$(view) && [ -n "$now" ] && v=$now
      stamp_v=$(stamp) recheck=0 next_view=$((SECONDS + 60))
      start_rdp
    fi
  elif [ "$seen" -ne 0 ]; then
    watched=-99
    if [ $((SECONDS - seen)) -ge "$idle_sec" ]; then
      seen=0
      if [ -n "$rdp" ]; then
        echo "no VNC client for ${idle_sec}s, stopping the RDP client"
        stop_rdp
        wait "$rdp" 2>/dev/null || true
        rdp=
      fi
    fi
  fi
  if [ -n "$rdp" ] && ! kill -0 "$rdp" 2>/dev/null; then
    # It dropped, or the layout changed: reconnect next round if a client is still there.
    wait "$rdp" 2>/dev/null || true
    rdp=
    nap 2
    continue
  fi
  if [ -n "$rdp" ]; then
    # A layout change shows up in KWin's outputs a few seconds after the files change.
    s=$(stamp)
    [ "$s" = "$stamp_v" ] || { stamp_v=$s; recheck=$((SECONDS + 10)) next_view=$SECONDS; }
    if [ "$SECONDS" -ge "$next_view" ]; then
      next_view=$((SECONDS + 60))
      [ "$SECONDS" -ge "$recheck" ] || next_view=$((SECONDS + 2))
      now=$(view) || now=
      if [ -n "$now" ] && [ "$now" != "$v" ]; then
        echo "layout changed: $v -> $now"
        v=$now
        stop_rdp
      fi
    fi
  fi
  if [ -n "$rdp" ] || [ "$seen" -ne 0 ]; then nap 1; else nap 5; fi
done
