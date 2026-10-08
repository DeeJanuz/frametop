#!/usr/bin/env bash
# Uninstall Frametop from the Steam Frame. In a terminal on the headset (Konsole in the desktop,
# or over SSH):
#
#   curl -fsSL https://deejanuz.github.io/frametop/uninstall.sh | bash
#
# or ~/frametop/uninstall.sh. It doesn't use the rest of the repo, so it also works when
# ~/frametop is gone or broken.
#
# It never stops what you're using now: the input relay carries the keyboard and mouse, and the
# desktop runs from the repo. So it goes in two steps:
#   1. Frametop stops starting. The launcher's Desktop entry goes back to the stock desktop, and
#      Frametop's Native Desktop entry, services, SteamVR driver, menu entries, and system files
#      (our eye tracker's frame grabber and the Bluetooth fixes, with sudo) are removed, and so
#      are the file capabilities of hand tracking's camera broker (ft-camd, with sudo). What runs
#      now keeps running until you restart the headset.
#   2. After the restart, run it again. It deletes the code (~/frametop, or the releases in
#      ~/.local/share/frametop/releases with their containers and images) and, if you want, your
#      settings and the build container.
# When nothing of Frametop is running, one run does both.
#
# Options (piped, they go after "bash -s --"):
#   --dir DIR    where the repo is, if not ~/frametop
#   --dry-run    show what it would do, and change nothing
set -euo pipefail
shopt -s nullglob

usage() {
  cat <<'EOF'
usage: uninstall.sh [--dir DIR] [--dry-run]
piped: curl -fsSL https://deejanuz.github.io/frametop/uninstall.sh | bash -s -- [options]
EOF
}

dry=0
apps=$HOME/.local/share/applications
override=$apps/deckard-nested-desktop.desktop
native_copy=$apps/native-deckard-nested-desktop.desktop
relay_unit=$HOME/.config/systemd/user/frametop-input-relay.service
driver=$HOME/.local/share/frametop/ft_pointer
releases=$HOME/.local/share/frametop/releases  # get.sh --release
vrpathreg=/opt/steamvr/bin/linuxarm64/vrpathreg
handsctl=$HOME/.local/bin/ft-handsctl
eyegrab_files=(/etc/systemd/system/frametop-eyegrab.service /etc/frametop/ft-eyegrab)
bt_files=(/etc/systemd/system/steamframe-bt-fixups.service /etc/systemd/system/bluetooth.service.d/steamframe.conf
          /etc/steamframe/bt-fixups.sh)

step() { printf '\n\033[1m== %s\033[0m\n' "$*"; }
run() {  # run a command, or with --dry-run, show it
  if [ "$dry" = 1 ]; then printf '  would run: %s\n' "$*"; else "$@"; fi
}
ask() {  # ask "question" default(y|n)
  local hint answer
  if [ "$dry" = 1 ]; then echo "$1 [dry run: yes]"; return 0; fi
  hint=$([ "$2" = y ] && echo "Y/n" || echo "y/N")
  read -r -p "$1 [$hint] " answer </dev/tty || answer=
  answer=${answer:-$2}
  [[ $answer =~ ^[Yy] ]]
}
exists() { local f; for f in "$@"; do [ -e "$f" ] && return 0; done; return 1; }
size() { du -shc "$@" 2>/dev/null | tail -1 | cut -f1; }

# Is this folder Frametop's code? Only then is it deleted.
is_repo() { [ "$1" != "$HOME" ] && [ -f "$1/desktops.sh" ] && [ -f "$1/session/frametop-session.sh" ]; }
find_repo() {
  local p
  [ -n "$dir" ] && { echo "$dir"; return; }
  # The launcher entry and the relay's unit point into the repo, until step 1 removes them.
  p=$(sed -n 's#^Exec=\(.*\)/session/frametop-session\.sh.*#\1#p' "$override" 2>/dev/null | head -1)
  [ -z "$p" ] && p=$(sed -n 's#^ExecStart=/usr/bin/python3 \(.*\)/input/input-relay\.py.*#\1#p' "$relay_unit" 2>/dev/null | head -1)
  [ -z "$p" ] && [ -f "${BASH_SOURCE[0]:-}" ] && p=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
  echo "${p:-$HOME/frametop}"
}

# Frametop's programs that are running now (step 1 leaves them running until the restart).
running() {
  local n out=()
  for n in ft-screens ft-pointer ft-powerd ft-eyegrab ft-camd ft-hands; do
    pgrep -x "$n" >/dev/null && out+=("$n")
  done
  pgrep -f '[i]nput/input-relay\.py' >/dev/null && out+=("input relay")
  pgrep -f '[g]aze/ft-gazed' >/dev/null && out+=("gaze service")
  pgrep -f '[f]rametop-session\.sh' >/dev/null && out+=("desktop session")
  echo "${out[*]}"
}

main() {
  local units=() entries=() names=() sys=0 repo now f
  while [ $# -gt 0 ]; do
    case $1 in
      --dir) dir=${2:?--dir needs a folder}; shift ;;
      --dry-run) dry=1 ;;
      -h|--help) usage; return 0 ;;
      *) echo "unknown option: $1" >&2; usage >&2; return 2 ;;
    esac
    shift
  done

  if ! { grep -qx 'ID=steamos' /etc/os-release && grep -qE '^VARIANT_ID="?vr"?$' /etc/os-release; } 2>/dev/null; then
    echo "This uninstalls Frametop from a Steam Frame (SteamOS, VR variant). Run it in a terminal on the headset." >&2
    return 1
  fi
  if [ "$dry" = 0 ] && ! { : </dev/tty; } 2>/dev/null; then
    echo "This asks questions, and there's no terminal to ask in. Run it in one (over SSH: ssh -t)." >&2
    return 1
  fi
  [ "$dry" = 1 ] && echo "Dry run: nothing changes."
  repo=$(find_repo)
  camd=$repo/hands/build/ft-camd
  camd_caps=$(getcap "$camd" 2>/dev/null || true)

  # Step 1: what makes Frametop start. Nothing here stops a running program.
  [ -f "$override" ] && grep -q 'Frametop' "$override" || override=
  [ -f "$native_copy" ] || native_copy=
  units=("$HOME"/.config/systemd/user/frametop-*.service)
  for f in ft-input-settings ft-display-settings ft-remote-displays ft-layout-reset ft-screens-toggle ft-remote-settings ft-gazeprobe; do
    [ -e "$apps/$f.desktop" ] && entries+=("$apps/$f.desktop")
  done
  [ -e "$apps/frametop-handrec.desktop" ] && entries+=("$apps/frametop-handrec.desktop")
  entries+=("$apps"/frametop-profile-*.desktop)  # one per layout profile (layout/ft_layout.py)
  [ -L "$handsctl" ] || handsctl=
  exists "${eyegrab_files[@]}" "${bt_files[@]}" && sys=1

  if [ -n "$override" ] || [ -n "$native_copy" ] || [ ${#units[@]} -gt 0 ] || [ ${#entries[@]} -gt 0 ] ||
     [ -d "$driver" ] || [ -n "$handsctl" ] || [ "$sys" = 1 ] || [ -n "$camd_caps" ]; then
    step "Step 1 of 2: stop Frametop from starting"
    echo "This removes:"
    [ -n "$override" ] && echo "  - the launcher's Desktop entry (Launch a program -> Desktop opens the stock desktop again)"
    [ -n "$native_copy" ] && echo "  - the launcher's Native Desktop entry (Desktop opens the same stock desktop then)"
    for f in "${units[@]}"; do echo "  - the service $(basename "$f")"; done
    [ -d "$driver" ] && echo "  - the 3D mouse's SteamVR driver (ft_pointer)"
    [ ${#entries[@]} -gt 0 ] && echo "  - ${#entries[@]} menu entries (Frametop Display Settings, Input Settings, ...)"
    [ -n "$handsctl" ] && echo "  - $handsctl"
    [ -n "$camd_caps" ] && echo "  - the hand camera broker's file capabilities (needs your password)"
    exists "${eyegrab_files[@]}" && echo "  - our eye tracker's frame grabber (a system service: needs your password)"
    exists "${bt_files[@]}" && echo "  - the Bluetooth fixes (system files: needs your password)"
    echo "What runs now keeps running until you restart the headset, so your keyboard, mouse, and"
    echo "this terminal keep working. Your settings and the code stay for now."
    ask "Uninstall Frametop?" n || { echo "Nothing changed."; return 0; }

    [ -n "$override" ] && run rm -f "$override"
    [ -n "$native_copy" ] && run rm -f "$native_copy"
    if [ ${#units[@]} -gt 0 ]; then
      for f in "${units[@]}"; do names+=("$(basename "$f")"); done
      run systemctl --user disable "${names[@]}" 2>/dev/null || true
      run rm -f "${units[@]}"
      run systemctl --user daemon-reload
    fi
    if [ -d "$driver" ]; then
      if [ -x "$vrpathreg" ]; then
        LD_LIBRARY_PATH=$(dirname "$vrpathreg") run "$vrpathreg" removedriver "$driver" ||
          echo "warning: SteamVR's vrpathreg couldn't unregister the driver; SteamVR may log that it's missing" >&2
      fi
      run rm -rf "$driver"
    fi
    [ ${#entries[@]} -gt 0 ] && run rm -f "${entries[@]}"
    [ -n "$handsctl" ] && run rm -f "$handsctl"
    if [ "$sys" = 1 ]; then
      echo "The system files need your password (sudo)."
      # $1 is ft-camd when it has capabilities to remove, else empty; the system files follow.
      if ! run sudo bash -c '
        camd=$1; shift
        for u in frametop-eyegrab steamframe-bt-fixups; do systemctl disable $u.service 2>/dev/null; done
        rm -f "$@"
        rmdir /etc/frametop /etc/steamframe /etc/systemd/system/bluetooth.service.d 2>/dev/null
        [ -n "$camd" ] && [ -x "$camd" ] && setcap -r "$camd" 2>/dev/null
        systemctl daemon-reload; true' sys "${camd_caps:+$camd}" "${eyegrab_files[@]}" "${bt_files[@]}"; then
        echo "warning: the system files weren't removed (no password?). Run this again to retry." >&2
      fi
    elif [ -n "$camd_caps" ]; then
      echo "ft-camd's file capabilities need your password (sudo) to remove."
      run sudo setcap -r "$camd" ||
        echo "warning: ft-camd keeps its capabilities (no password?). Run this again to retry." >&2
    fi
    [ "$dry" = 1 ] || echo "Frametop no longer starts."
  fi

  # Step 2: delete what's left, once nothing of Frametop runs.
  now=$(running)
  if [ -n "$now" ]; then
    step "Restart the headset to finish"
    echo "Still running from before: $now."
    echo "They stop when the headset restarts. After that, run this again to delete the code"
    echo "($repo) and, if you want, your settings and the build container:"
    echo
    if [ "$repo" = "$HOME/frametop" ]; then
      echo "  curl -fsSL https://deejanuz.github.io/frametop/uninstall.sh | bash"
    else
      echo "  curl -fsSL https://deejanuz.github.io/frametop/uninstall.sh | bash -s -- --dir $(printf %q "$repo")"
    fi
    echo
    if ask "Restart the headset now? This closes everything open, in VR and on the desktop." n; then
      run systemctl reboot || echo "Couldn't restart it from here: restart the headset from Steam's power menu." >&2
    fi
    [ "$dry" = 1 ] || return 0
    echo "Dry run: after the restart, step 2 would go like this."
  fi

  step "Step 2 of 2: delete what's left"
  if [ -d "$releases" ]; then
    local boxes=() images=() b
    for f in "$releases"/*/.frametop-release; do
      boxes+=("$(sed -n 's/^BOX=//p' "$f")")
      images+=("$(sed -n 's/^IMAGE=//p' "$f")")
    done
    echo "Frametop's releases: their files in $releases ($(size "$releases")), and the container and"
    echo "image of each (${#boxes[@]}; an image is about 3 GB)."
    if ask "Delete the releases, their containers, and their images?" y; then
      for b in "${boxes[@]}"; do
        [[ $b =~ ^frametop-[A-Za-z0-9_.-]+$ ]] && { run podman rm -f "$b" 2>/dev/null || true; }
      done
      for b in "${images[@]}"; do
        [[ $b == *frametop*@sha256:* ]] && { run podman rmi "$b" 2>/dev/null || true; }
      done
      run rm -rf "$releases"
    fi
  fi
  if [[ $repo == "$releases"/* ]]; then
    :  # a release: above
  elif [ -d "$repo" ] && is_repo "$repo"; then
    if [ -f "$repo/.git" ]; then
      echo "Leaving $repo: it's a git worktree. Remove it with git worktree remove."
    else
      local def=y
      if [ -n "$(git -C "$repo" status --porcelain --untracked-files=no 2>/dev/null)" ] ||
         [ -n "$(git -C "$repo" log --branches --not --remotes --oneline 2>/dev/null | head -1)" ]; then
        echo "$repo has changes of its own (git -C $repo status)."
        def=n
      fi
      if ask "Delete the Frametop code in $repo ($(size "$repo"))?" "$def"; then
        cd "$HOME"
        run rm -rf "$repo"
      fi
    fi
  elif [ -e "$repo" ]; then
    echo "Leaving $repo: it doesn't look like Frametop's code."
  fi

  # Rebuilt by the desktop at each start, so nothing to keep.
  run rm -rf "$HOME/.local/share/frametop/apps" "$HOME/.local/share/kwin/decorations/kwin4_decoration_qml_frametop" \
    "$HOME/.cache/frametop"

  local settings=("$HOME"/.config/frametop.conf* "$HOME"/.config/frametop-*.json* "$HOME/.config/frametop-remote"
                  "$HOME/.config/frametop" "$HOME/.local/state/frametop" "$HOME/.local/share/frametop-stream")
  local kept=()
  for f in "${settings[@]}"; do [ -e "$f" ] && kept+=("$f"); done
  if [ ${#kept[@]} -gt 0 ]; then
    echo "Your settings: the screen layout and profiles, button maps, gaze calibration, the remote"
    echo "desktop password, the remote displays' pairings and sign-ins (their computers keep Frametop's"
    echo "clients and token until you remove them in their Web UI), and the Frametop desktop's own Plasma"
    echo "setup (${kept[*]/#$HOME/\~})."
    ask "Delete your settings too? Keep them to pick up where you left off if you reinstall." n &&
      run rm -rf "${kept[@]}"
  fi

  local recs=()
  for f in "$HOME/.local/share/frametop/eyes" "$HOME/.local/share/frametop/hands"; do [ -e "$f" ] && recs+=("$f"); done
  if [ ${#recs[@]} -gt 0 ]; then
    ask "Delete your eye and hand recordings in ~/.local/share/frametop ($(size "${recs[@]}"))?" n &&
      run rm -rf "${recs[@]}"
  fi
  [ "$dry" = 1 ] || rmdir "$HOME/.local/share/frametop" 2>/dev/null || true

  if command -v podman >/dev/null && podman container exists dev 2>/dev/null; then
    echo "The build container (dev) holds Frametop's compilers and libraries, 1-2 GB. If you put"
    echo "anything else in it, that goes too."
    if ask "Delete the build container?" n; then
      run "$HOME/.local/bin/distrobox" rm --force dev </dev/null
      run podman image rm registry.fedoraproject.org/fedora-toolbox:44 >/dev/null 2>&1 || true
      echo "distrobox stays in ~/.local/bin, for any other containers. To remove it too:"
      echo "  ~/dev/src/distrobox/uninstall --prefix ~/.local"
    fi
  fi

  step "Done"
  echo "Frametop is uninstalled."
}

dir=
main "$@"
