#!/bin/bash
# Runs on the Frame host (not in the container). Starts a Plasma desktop with several
# screens, each its own SteamVR panel. Modeled on /usr/bin/steamos-nested-desktop, which
# does the same with one screen in gamescope.
#
# Backends (BACKEND in ~/.config/frametop.conf):
#   screens    (default) ft-screens (screens), our own compositor: KWin opens a
#              window per screen, ft-screens gives each its size (from the layout, see
#              layout) and shows it as its own panel. Any resolution and shape.
#   gamescope  gamescope in PerWindow mode: every screen one size, at most 1920x1080
#              worth of pixels, panels owned by the SteamVR dashboard.
#
# Settings come from ~/.config/frametop.conf (see frametop.conf.example) and, for the
# screens backend, ~/.config/frametop-layout.json (Frametop Display Settings writes both).
# FT_BACKEND, FT_SCREENS, FT_WIDTH, FT_HEIGHT, and FT_PHYS_WIDTH override them.
set -eu

here=$(dirname "$(readlink -f "$0")")

# Started from the VR launcher, the desktop inherits the Steam client's environment. Apps in
# it should see the system as a normal login does, so drop the client's runtime: its
# LD_LIBRARY_PATH put Steam's own libraries ahead of the system's (Steam's libavcodec has no
# H.264 decoder, so VLC couldn't play most videos), and its overlay and launch settings are
# meant for games. SteamOS's own defaults (/usr/share/deckard/mesavars.sh) stay. The
# gamescope session also puts QT_IM_MODULE=xim and GTK_IM_MODULE=xim in the systemd user
# environment, and with those, Qt and GTK apps never tell KWin a text field has focus, so
# Frametop's keyboard (input/ft-textinput) never opens for them.
for var in $(compgen -e); do
  case $var in
    LD_LIBRARY_PATH | LD_PRELOAD | STEAM_* | Steam* | SRT_* | PRESSURE_VESSEL_* | MANGOHUD_* | \
      ENABLE_VK_LAYER_VALVE_steam_overlay_* | STEAMVIDEOTOKEN | QT_IM_MODULE | GTK_IM_MODULE | \
      XMODIFIERS) unset "$var" ;;
  esac
done

conf=$HOME/.config/frametop.conf
BACKEND=screens SCREENS=2 WIDTH=1920 HEIGHT=1080 PHYS_WIDTH=1.6 REMOTE=0 FLOAT_SLOTS=8 FLOAT_MARGIN=300
# shellcheck disable=SC1090
[ -f "$conf" ] && . "$conf"
backend=${FT_BACKEND:-$BACKEND}
screens=${FT_SCREENS:-$SCREENS}
width=${FT_WIDTH:-$WIDTH}
height=${FT_HEIGHT:-$HEIGHT}
phys_width=${FT_PHYS_WIDTH:-$PHYS_WIDTH}
remote=${FT_REMOTE:-$REMOTE}
# Floating windows (screens backend): KWin gets this many spare outputs after the screens,
# and ft-floatd floats a window on each (docs/floating-windows.md). Changing it takes a
# desktop restart.
float_slots=${FT_FLOAT_SLOTS:-$FLOAT_SLOTS}
[[ $float_slots =~ ^[0-9]+$ ]] || float_slots=8
[ "$float_slots" -le 16 ] || float_slots=16
[ "$backend" = screens ] || float_slots=0
if [ "$backend" = gamescope ] && [ $((width * height)) -gt $((1920 * 1080)) ]; then
  # gamescope's VR backend aborts above 1920x1080 worth of pixels (its upload buffer;
  # see docs/design.md). Shrink a bigger size to fit, keeping its shape.
  read -r width height < <(awk -v w="$width" -v h="$height" 'BEGIN { k = sqrt(1920 * 1080 / (w * h));
    printf "%d %d\n", int(w * k / 8) * 8, int(h * k / 8) * 8 }')
  echo "frametop: resolution too big for gamescope's VR mode; using ${width}x${height}" >&2
fi

if [ "${1:-}" != --inner ]; then
  # One instance at a time. The launcher can be clicked twice.
  if pgrep -f '[v]r-overlay-key frametop ' >/dev/null || pgrep -x ft-screens >/dev/null; then
    echo "Frametop is already running" >&2
    exit 0
  fi
  # A SteamOS update could move either of the files sourced here. The desktop still
  # starts without them (scripts/update-check.py reports it).
  if [ -r /usr/share/deckard/mesavars.sh ]; then
    set -a; . /usr/share/deckard/mesavars.sh; set +a
  else
    echo "frametop: no /usr/share/deckard/mesavars.sh, starting without SteamOS's Mesa settings" >&2
  fi
  # Flatpak apps (Chromium) publish their launcher entries under the Flatpak
  # exports dirs. SSH and launcher environments may lack XDG_DATA_DIRS, and then
  # Plasma can't find them and opens Discover instead.
  export XDG_DATA_DIRS=${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
  if [ -r /etc/profile.d/flatpak.sh ]; then
    set +u; . /etc/profile.d/flatpak.sh; set -u
  else
    echo "frametop: no /etc/profile.d/flatpak.sh, so Flatpak apps may open Discover instead" >&2
  fi
  # Arrange the screens once they're up: in the profile this desktop starts with (FT_PROFILE,
  # from a profile's launcher entry, or the default profile), which also opens its apps, or
  # else in the saved layout (skipped when auto-arrange is off). docs/profiles.md.
  setsid "$here/../layout/ft-layout" start --wait 90 > /tmp/frametop-layout.log 2>&1 < /dev/null &

  if [ "$backend" = gamescope ]; then
    export ENABLE_GAMESCOPE_WSI=1 GAMESCOPE_MANGOAPP_SOCKET_DISABLE=1
    exec gamescope --backend openvr --virtual-connector-strategy PerWindow \
      -W "$width" -H "$height" -w "$width" -h "$height" \
      --vr-overlay-key frametop --vr-overlay-default-name frametop \
      --vr-overlay-physical-width "$phys_width" \
      --vr-overlay-show-immediately --vr-overlay-enable-click-stabilization \
      --vr-overlay-enable-control-bar --vr-overlay-enable-control-bar-keyboard \
      --expose-wayland \
      --cursor-hotspot 5,3 --cursor /usr/share/steamos/steamos-cursor.png \
      -- "$0" --inner
  fi

  # ft-screens runs in the dev container (it's built against Fedora's wlroots); KWin and
  # Plasma stay on the host and connect to its socket. FRAMETOP_SCREENS_BIN names a build
  # that runs on the host instead (the Nix package's, nix/README.md).
  socket=ft-screens-0
  read -ra screen_args <<< "$("$here/../layout/ft-layout" screen-args)"
  export FT_SCREEN_COUNT=$(( ${#screen_args[@]} / 2 )) FT_FLOAT_SLOTS=$float_slots
  screens_bin=${FRAMETOP_SCREENS_BIN:-}
  if [ -n "$screens_bin" ]; then
    screens_cmd=("$screens_bin")
  else
    "$here/../scripts/container-up.sh"  # not owned by this desktop, or stopping it would stop the container
    screens_cmd=("$HOME/.local/bin/distrobox" enter dev -- "$here/../screens/build/ft-screens")
  fi
  "${screens_cmd[@]}" --socket "$socket" \
    "${screen_args[@]}" --spares "$float_slots" > /tmp/frametop-screens.log 2>&1 < /dev/null &
  stop_screens() { pkill -x ft-screens 2>/dev/null || true; }
  trap stop_screens EXIT
  for _ in $(seq 100); do [ -S "$XDG_RUNTIME_DIR/$socket" ] && break; sleep 0.2; done
  [ -S "$XDG_RUNTIME_DIR/$socket" ] || { echo "ft-screens didn't start (see /tmp/frametop-screens.log)" >&2; exit 1; }
  WAYLAND_DISPLAY=$XDG_RUNTIME_DIR/$socket "$0" --inner
  exit
fi

# Inside the host compositor (ft-screens or gamescope) from here on. The desktop isn't a
# gamescope client with ft-screens, so the gamescope session's Vulkan layer stays off, and
# the gamescope session's portal config isn't Plasma's.
[ "$backend" = gamescope ] || unset ENABLE_GAMESCOPE_WSI
unset XDG_DESKTOP_PORTAL_DIR
[ "$backend" = gamescope ] || screens=${FT_SCREEN_COUNT:-$screens}

host_runtime=$XDG_RUNTIME_DIR
runtime=$host_runtime/frametop

cleanup() {
  "$here/remote-ctl.sh" stop
  fusermount3 -u -z "$runtime/doc" 2>/dev/null || true
  umount --recursive "$runtime" 2>/dev/null || true
  rm -rf "$runtime"
}
trap cleanup EXIT
cleanup
mkdir -m 0700 "$runtime" "$runtime/pulse" "$runtime/bin"
ln -s "$host_runtime/pulse/native" "$runtime/pulse/native"
ln -s "$host_runtime"/pipewire* "$runtime/"

# The file picker gives Flatpak apps paths in our document portal, $runtime/doc/ID/NAME.
# Sandboxes only see the portal at /run/flatpak/doc, and their /run/user/UID is a
# private folder ($runtime/.flatpak/APP/xdg-run). So a saved download or an upload
# lands in an empty folder the app creates in there and never leaves the sandbox.
# Link that path to the portal in each app's folder. xdg-desktop-portal after 1.22.1
# returns /run/flatpak/doc paths itself (upstream commit 69ba5e1). Apps installed
# while the desktop runs get the link at the next start.
sandbox_runtime=/run/user/$(id -u)
if [[ $runtime == "$sandbox_runtime"/* ]]; then
  while read -r app; do
    app_doc=$runtime/.flatpak/$app/xdg-run/${runtime#"$sandbox_runtime"/}/doc
    (umask 077; mkdir -p "$(dirname "$app_doc")" && ln -sfn /run/flatpak/doc "$app_doc") || true
  done < <(flatpak list --app --columns=application 2>/dev/null)
fi

# plasma-session starts KWin through kwin_wayland_wrapper. Shadow it to add our outputs.
# With ft-screens the size is only the starting one: ft-screens sets each screen's own.
# Our input method tells the input relay when a text field has focus, for SteamVR's
# keyboard (input/ft-textinput).
textinput=$(readlink -f "$here/../input/ft-textinput")
cat > "$runtime/bin/kwin_wayland_wrapper" <<EOF
#!/bin/sh
exec /usr/bin/kwin_wayland_wrapper --width $width --height $height --output-count $((screens + float_slots)) \\
  --no-lockscreen --inputmethod $textinput "\$@"
EOF
chmod +x "$runtime/bin/kwin_wayland_wrapper"
export PATH=$runtime/bin:$PATH

# Keep the host compositor's Wayland socket reachable after moving XDG_RUNTIME_DIR.
case ${WAYLAND_DISPLAY:-gamescope-0} in
  /*) ;;
  *) export WAYLAND_DISPLAY=$host_runtime/${WAYLAND_DISPLAY:-gamescope-0} ;;
esac
export XDG_RUNTIME_DIR=$runtime

# Separate Plasma/KWin config and state, so this session and the built-in
# desktop never overwrite each other's screen layout or panels.
export XDG_CONFIG_HOME=$HOME/.config/frametop
export XDG_STATE_HOME=$HOME/.local/state/frametop
mkdir -p "$XDG_CONFIG_HOME" "$XDG_STATE_HOME"

# Apps started here inherit both, so everything in them but Plasma's own files links to
# the real folders: the apps keep their config, logins, and history.
"${FRAMETOP_PYTHON:-python3}" "$here/config_links.py" "$HOME/.config" "$XDG_CONFIG_HOME" config || true
"${FRAMETOP_PYTHON:-python3}" "$here/config_links.py" "$HOME/.local/state" "$XDG_STATE_HOME" state || true

# Remote desktop over VNC: session/remote-desktop.sh captures the desktop with
# krdp on 127.0.0.1, and session/vnc-bridge.sh re-serves its primary screen over VNC. krdpserver runs from the container, so KWin can't
# match it to an installed app. KWin's permission check for screencast and fake
# input is turned off for this nested session only, and so is the check on KWin's
# D-Bus screenshot interface, which scripts use to see the screens without the headset.
# The marker lets remote-ctl.sh (and Frametop Remote Access) start and stop it later in
# this session; a desktop started without it can't capture.
if [ "$remote" = 1 ]; then
  export KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1
  touch "$runtime/remote-capable"
  "$here/remote-ctl.sh" start
fi

# ft-floatd (floating windows) runs inside the Plasma session, on its D-Bus: started from
# the session's autostart, which only this desktop reads (XDG_CONFIG_HOME above).
autostart=$XDG_CONFIG_HOME/autostart/frametop-floatd.desktop
if [ "$float_slots" -gt 0 ]; then
  mkdir -p "$(dirname "$autostart")"
  cat > "$autostart" <<EOF
[Desktop Entry]
Type=Application
Name=Frametop floating windows
Exec=sh -c 'exec "$here/../float/ft-floatd" --screens $screens --slots $float_slots > /tmp/frametop-floatd.log 2>&1'
X-KDE-autostart-phase=2
NoDisplay=true
EOF
else
  rm -f "$autostart"
fi

# Launch as Standalone in every app's right-click menu (float/ft_apps.py): copies of the
# apps' desktop files with that action, first in XDG_DATA_DIRS, so only this desktop sees
# them. Written now, before Plasma reads them; ft-floatd keeps them up to date.
if [ "$float_slots" -gt 0 ]; then
  "${FRAMETOP_PYTHON:-python3}" "$here/../float/ft_apps.py" >/dev/null 2>&1 || true
  export XDG_DATA_DIRS=$HOME/.local/share/frametop/apps:${XDG_DATA_DIRS:-/usr/local/share:/usr/share}
fi

# With floating windows, the session's windows get Frametop's own decoration: Breeze's look
# plus a float button left of Close (decoration/, a QML decoration KWin's Aurorae engine
# loads; docs/floating-windows.md). It's copied, not linked: KPackage doesn't list linked
# packages. Desktop Mode keeps its own kwinrc, so it keeps Breeze.
deco=kwin4_decoration_qml_frametop
deco_dir=${XDG_DATA_HOME:-$HOME/.local/share}/kwin/decorations/$deco
kwinrc=$XDG_CONFIG_HOME/kwinrc
if [ "$float_slots" -gt 0 ]; then
  rm -rf "$deco_dir" "$deco_dir"_try*  # (decoration/apply.sh's copies)
  mkdir -p "$(dirname "$deco_dir")"
  cp -r "$here/../decoration" "$deco_dir"
  chmod -R u+w "$deco_dir"  # (a read-only source, like the Nix store, would block the rm above next time)
  rm -f "$deco_dir/apply.sh"
  kwriteconfig6 --file "$kwinrc" --group org.kde.kdecoration2 --key library org.kde.kwin.aurorae
  kwriteconfig6 --file "$kwinrc" --group org.kde.kdecoration2 --key theme "$deco"
elif [ "$(kreadconfig6 --file "$kwinrc" --group org.kde.kdecoration2 --key theme)" = "$deco" ]; then
  kwriteconfig6 --file "$kwinrc" --group org.kde.kdecoration2 --key library --delete
  kwriteconfig6 --file "$kwinrc" --group org.kde.kdecoration2 --key theme --delete
fi

# Profiles reopen apps (docs/profiles.md), so Plasma's own session restore stays off here;
# with both, apps would open twice.
kwriteconfig6 --file "$XDG_CONFIG_HOME/ksmserverrc" --group General --key loginMode emptySession

# FRAMETOP_STARTPLASMA: the host's Plasma, when PATH might find another one first.
dbus-run-session "${FRAMETOP_STARTPLASMA:-startplasma-wayland}"
