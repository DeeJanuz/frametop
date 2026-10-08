#!/usr/bin/env bash
# The runtime trial (HEADSET-TESTS.md, "Frametop from the image"): switch this Frame's Frametop
# to a release, installed from its Frametop.zip, and back to what was there.
#   pack/trial.sh on DIR    # DIR: the unpacked zip's Frametop folder (install-release.sh in it)
#   pack/trial.sh desktop   # restart the VR desktop from the release (ft-screens in its container)
#   pack/trial.sh status    # what runs where
#   pack/trial.sh off       # put it all back
# on saves what the release's install.sh replaces to ~/.local/state/frametop-trial/saved: the
# frametop-* user units with their drop-ins and .wants links, the launcher and menu entries, the
# SteamVR driver's folder and registry, frametop.conf, and the desktop's shortcuts file. It
# moves the drop-ins aside (they would override the release's units), runs the release's
# install-release.sh --yes --no-eye-tracker --no-bluetooth (no sudo: the installed eye grabber
# stays), and restarts SteamVR so it loads the release's driver and services.
# off removes those files and puts the saved ones back, stops the release's container, and
# restarts SteamVR. The release stays in ~/.local/share/frametop/releases for another try.
# Restarting SteamVR closes everything open in VR.
set -euo pipefail
shopt -s nullglob

self=$(readlink -f "$0")
state=$HOME/.local/state/frametop-trial
saved=$state/saved
export XDG_RUNTIME_DIR=/run/user/$(id -u)
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus

# What install.sh changes (pack/README.md, Releases), relative to the home folder, as globs.
paths() {
  cat <<'EOF'
.config/systemd/user/frametop-*.service
.config/systemd/user/frametop-*.service.d
.config/systemd/user/*.wants/frametop-*
.local/share/applications/deckard-nested-desktop.desktop
.local/share/applications/native-deckard-nested-desktop.desktop
.local/share/applications/ft-*.desktop
.local/share/applications/frametop-profile-*.desktop
.local/share/frametop/ft_pointer
.config/openvr/openvrpaths.vrpath
.config/frametop.conf
.config/frametop/kglobalshortcutsrc
EOF
}

restart_steamvr() {
  echo "restarting SteamVR"
  systemctl --user restart steamvr.service
  for _ in $(seq 60); do
    systemctl --user -q is-active frametop-pointer.service && break
    sleep 1
  done
}

case ${1:-status} in
  on)
    src=$(cd "${2:?usage: pack/trial.sh on DIR (the Frametop folder of the unpacked zip)}" && pwd)
    [ -x "$src/install-release.sh" ] || { echo "$src/install-release.sh isn't there" >&2; exit 1; }
    [ -e "$saved" ] && { echo "a trial is on already: pack/trial.sh off first" >&2; exit 1; }
    mkdir -p "$saved"
    cd "$HOME"
    while read -r glob; do
      for p in $glob; do
        [ -e "$p" ] || [ -L "$p" ] || continue  # a plain name stays as is when it's missing
        cp -a --parents "$p" "$saved/"
      done
    done < <(paths)
    echo "saved $(find "$saved" -mindepth 1 -not -type d | wc -l) files in $saved"
    rm -rf .config/systemd/user/frametop-*.service.d
    systemctl --user daemon-reload
    "$src/install-release.sh" --yes --no-eye-tracker --no-bluetooth
    restart_steamvr
    "$self" status ;;
  desktop)
    release=$HOME/.local/share/frametop/releases/current
    [ -e "$saved" ] && [ -x "$release/desktops.sh" ] || { echo "no trial on" >&2; exit 1; }
    "$release/desktops.sh" restart ;;
  off)
    [ -d "$saved" ] || { echo "no trial on (nothing saved in $saved)" >&2; exit 1; }
    cd "$HOME"
    while read -r glob; do
      for p in $glob; do rm -rf "$p"; done
    done < <(paths)
    cp -a "$saved/." "$HOME/"
    systemctl --user daemon-reload
    restart_steamvr  # first: the release's programs run in its container until SteamVR stops them
    pkill -x ft-screens || true  # the VR desktop from the release, if it's still up
    for box in $(podman ps --format '{{.Names}}' | grep -E '^frametop-[0-9a-f]{12}$' || true); do
      echo "stopping $box"
      podman stop -t 5 "$box" >/dev/null || echo "couldn't stop $box (podman stop $box)" >&2
    done
    mv "$saved" "$state/restored-$(date +%Y%m%d-%H%M%S)"
    echo "back as before; restart the VR desktop from Launch a program to leave the release's"
    "$self" status ;;
  status)
    [ -d "$saved" ] && echo "trial: ON (saved in $saved)" || echo "trial: off"
    for u in frametop-input-relay frametop-pointer frametop-power frametop-gaze frametop-desktop; do
      printf '%-22s %-9s %s\n' "$u" "$(systemctl --user is-active $u.service)" \
        "$(systemctl --user show -P ExecStart $u.service | grep -o 'argv\[\]=[^;]*' | cut -c8- | cut -c1-110)"
    done
    for name in ft-pointer ft-powerd ft-gaze ft-eyes ft-screens; do
      if [ "$name" = ft-eyes ]; then pids=$(pgrep -f "[/]ft-eyes " || true); else pids=$(pgrep -x "$name" || true); fi
      for pid in $pids; do
        id=$(grep -o 'libpod-[0-9a-f]\{12\}' "/proc/$pid/cgroup" 2>/dev/null | head -1 | cut -c8-) || id=
        box=host
        [ -n "$id" ] && box=$(podman ps --filter "id=$id" --format '{{.Names}}' 2>/dev/null || echo "$id")
        printf '%-11s pid %-7s in %-22s %s\n' "$name" "$pid" "$box" \
          "$(tr '\0' ' ' </proc/$pid/cmdline 2>/dev/null | grep -o '/home/[^ ]*' | head -1 || true)"
      done
    done
    readlink -f "$HOME/.local/share/frametop/releases/current" 2>/dev/null | sed 's/^/release: /' || true ;;
  *) sed -n '2,6p' "$0" >&2; exit 2 ;;
esac
