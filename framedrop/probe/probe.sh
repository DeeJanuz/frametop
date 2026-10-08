#!/usr/bin/env bash
# FrameDrop proof of concept, step 1: run as a Steam "Devkit Game" (what FrameDrop installs a
# Linux zip as) and record what an installer started from Steam could use: host or Steam Linux
# Runtime container, podman, the user's systemd, git, the network, GTK, and a window on screen.
# Register and start it: framedrop/devkit.sh add FrametopProbe framedrop/probe "./probe.sh native"
#                       framedrop/devkit.sh run FrametopProbe
# Usage: probe.sh LABEL
# Writes ~/.cache/frametop-framedrop/probe-LABEL.log, replacing the previous one.
label=${1:-unlabeled}
out=$HOME/.cache/frametop-framedrop
mkdir -p "$out"
log=$out/probe-$label.log
exec >"$log" 2>&1

here=$(cd "$(dirname "$0")" && pwd)
section() { printf '\n== %s\n' "$1"; }
have() { command -v "$1" >/dev/null 2>&1; }
check() { # check NAME CMD...: one line, ok or failed with the exit status
  local name=$1; shift
  if out_=$(timeout 20 "$@" 2>&1); then echo "ok      $name: $(echo "$out_" | head -1)"
  else echo "FAILED  $name (exit $?): $(echo "$out_" | head -2 | tr '\n' ' ')"; fi
}

section "when, who, where"
date -Is
echo "label=$label uid=$(id -u) user=$(id -un) pwd=$PWD here=$here"
echo "args: $*"
grep -E '^(ID|VARIANT_ID|VERSION_ID|BUILD_ID|PRETTY_NAME)=' /etc/os-release

section "container?"
for p in /run/pressure-vessel /.flatpak-info /run/host/os-release; do
  [ -e "$p" ] && echo "present: $p" || echo "absent:  $p"
done
echo "container=${container-} PRESSURE_VESSEL_RUNTIME=${PRESSURE_VESSEL_RUNTIME-}"
[ -e /run/host/os-release ] && grep -E '^(ID|VARIANT_ID)=' /run/host/os-release

section "environment"
env | grep -E '^(PATH|HOME|XDG_[A-Z_]+|DISPLAY|WAYLAND_DISPLAY|DBUS_SESSION_BUS_ADDRESS|SteamAppId|SteamGameId|STEAM_COMPAT_[A-Z_]+|LD_LIBRARY_PATH|LD_PRELOAD|SDL_[A-Z_]+|GDK_BACKEND|ENABLE_[A-Z_]+|PRESSURE_VESSEL_[A-Z_]+)=' | sort
echo "process tree:"
pid=$$
for _ in 1 2 3 4 5 6 7 8; do
  [ "$pid" -le 1 ] 2>/dev/null && break
  printf '  %s %s\n' "$pid" "$(tr '\0' ' ' </proc/"$pid"/cmdline 2>/dev/null | cut -c1-200)"
  pid=$(awk '/^PPid:/{print $2}' /proc/"$pid"/status 2>/dev/null)
done

# Steam adds its overlay (LD_PRELOAD) and runtime libraries to every child. An installer has
# to drop them before running host tools, as the checks below do.
unset LD_PRELOAD
LD_LIBRARY_PATH=$(printf %s "${LD_LIBRARY_PATH-}" | tr ':' '\n' | grep -v -e '/Steam/' -e '^$' -e 'x86_64' -e 'i386' | paste -sd:)
[ -n "$LD_LIBRARY_PATH" ] && export LD_LIBRARY_PATH || unset LD_LIBRARY_PATH
PATH=$(printf %s "$PATH" | tr ':' '\n' | grep -v '/Steam/' | paste -sd:)
echo "cleaned: PATH=$PATH LD_LIBRARY_PATH=${LD_LIBRARY_PATH-}"

section "tools"
for t in bash git curl python3 podman distrobox systemctl systemd-run busctl gdbus flatpak-spawn \
         steam-runtime-launch-client konsole zenity kdialog; do
  printf '%-28s %s\n' "$t" "$(command -v "$t" || echo -)"
done

section "what an installer needs"
check "home writable" sh -c 'f=$HOME/.cache/frametop-framedrop/.w && : >"$f" && rm "$f" && echo yes'
check "~/frametop visible" sh -c 'ls -d "$HOME/frametop" && git -C "$HOME/frametop" log -1 --format=%h'
have git && check "git ls-remote github" git ls-remote --heads https://github.com/DeeJanuz/frametop.git experimental
have curl && check "curl get.sh" sh -c 'curl -fsSL https://deejanuz.github.io/frametop/get.sh | head -1'
have podman && check "podman ps" podman ps --format '{{.Names}}'
have distrobox && check "distrobox list" distrobox list
have systemctl && check "systemctl --user" systemctl --user is-system-running
have systemctl && check "user units visible" systemctl --user is-enabled frametop-input-relay.service
have python3 && check "python3 gi Gtk 4" python3 -c 'import gi; gi.require_version("Gtk","4.0"); gi.require_version("Adw","1"); from gi.repository import Gtk, Adw; print(Gtk.get_major_version(), Gtk.get_minor_version())'

section "escape to the host (needed if this is a container)"
# A transient user unit runs in the host's user manager, outside any container.
if have systemd-run; then
  rm -f "$out/escape-$label"
  check "systemd-run --user" systemd-run --user --wait --collect --quiet -- \
    sh -c "grep -E '^(ID|VARIANT_ID)=' /etc/os-release > '$out/escape-$label'; command -v podman git >> '$out/escape-$label'"
  [ -s "$out/escape-$label" ] && sed 's/^/  host says: /' "$out/escape-$label"
fi
if have busctl; then
  check "busctl --user (systemd1)" busctl --user get-property org.freedesktop.systemd1 /org/freedesktop/systemd1 org.freedesktop.systemd1.Manager Version
fi

section "window"
# A plain GTK 4 window for 20 s. Watch for it on the Frame; "window: mapped" means GTK showed it.
if have python3; then
  timeout 40 python3 - <<'EOF'
import sys
try:
    import gi
    gi.require_version("Gtk", "4.0")
    from gi.repository import Gtk, GLib
except Exception as e:
    print("window: no GTK 4:", e); sys.exit(0)
app = Gtk.Application(application_id="io.github.deejanuz.FrametopProbe")
def on_activate(app):
    w = Gtk.ApplicationWindow(application=app, title="Frametop installer probe")
    w.set_default_size(640, 240)
    w.set_child(Gtk.Label(label="Frametop installer probe\n\nIf you can read this, a FrameDrop install can show progress.\nThis window closes in 20 seconds."))
    w.connect("map", lambda *_: print("window: mapped", flush=True))
    w.present()
    GLib.timeout_add_seconds(20, app.quit)
app.connect("activate", on_activate)
print("window: display", Gtk.Widget.get_display(Gtk.Label()) if hasattr(Gtk.Widget, "get_display") else "?", flush=True)
app.run([])
print("window: closed")
EOF
  echo "window exit: $?"
else
  echo "window: no python3"
fi

section "done"
date -Is
