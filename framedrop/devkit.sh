#!/usr/bin/env bash
# Do on the Frame what FrameDrop does from a PC: copy a folder into ~/devkit-game/NAME and
# register it with Steam as a "Devkit Game" (Valve's devkit-utils, the same calls FrameDrop and
# the SteamOS Devkit Client make over SSH). For testing the FrameDrop installer and the probe
# without a PC. Steam must be running, and Developer Mode on.
#
# Usage: devkit.sh add NAME DIR COMMAND [--compat TOOL]
#        devkit.sh run NAME       # start it, as the library's Play button does
#        devkit.sh remove NAME    # delete the folder and the Steam entry
#        devkit.sh list
#
# NAME can't contain "-": Steam answers "missing/invalid arguments". COMMAND is relative to
# the folder, like FrameDrop's start command ("./probe.sh native"). --compat sets the runtime
# (SteamLinuxRuntime_4-arm64, say); on SteamOS 0.3.0 Steam recorded it but still ran the
# title on the host. There's no environment option: this Steam ignores the devkit env file.
#
# devkit-utils is Valve's (LGPL, gitlab.steamos.cloud/devkit/steamos-devkit). FrameDrop and
# the devkit client copy it to ~/devkit-utils; if that isn't there, it's fetched at a pinned
# commit into ~/.cache/frametop-framedrop.
set -euo pipefail

devkit_rev=a00ceb7d91ea44a0c3e714a91a06417d6e5cdb33
cache=$HOME/.cache/frametop-framedrop

usage() { sed -n '7,10p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

utils() {
  if [ -e "$HOME/devkit-utils/steam-client-create-shortcut" ]; then
    echo "$HOME/devkit-utils"; return
  fi
  if [ ! -e "$cache/devkit-utils/steam-client-create-shortcut" ]; then
    echo "fetching devkit-utils ($devkit_rev)" >&2
    local tmp
    tmp=$(mktemp -d)
    git -C "$tmp" init -q
    git -C "$tmp" fetch -q --depth 1 https://gitlab.steamos.cloud/devkit/steamos-devkit.git "$devkit_rev"
    git -C "$tmp" checkout -q FETCH_HEAD
    mkdir -p "$cache"
    rm -rf "$cache/devkit-utils"
    cp -r "$tmp/client/devkit-utils" "$cache/devkit-utils"
    rm -rf "$tmp"
  fi
  echo "$cache/devkit-utils"
}

[ $# -ge 1 ] || usage
cmd=$1; shift
case $cmd in
  add)
    [ $# -ge 3 ] || usage
    name=$1 src=$2 start=$3; shift 3
    case $name in *-*) echo "NAME can't contain '-' (Steam refuses it)" >&2; exit 2 ;; esac
    compat=
    while [ $# -gt 0 ]; do
      case $1 in
        --compat) compat=${2:?}; shift ;;
        *) usage ;;
      esac
      shift
    done
    u=$(utils)
    dir=$(python3 "$u/steamos-prepare-upload" --gameid "$name" | python3 -c 'import json,sys; print(json.load(sys.stdin)["directory"])')
    # FrameDrop rsyncs the unpacked zip; --delete as a clean upload would.
    rsync -a --delete "$src/" "$dir/"
    parms=$(python3 - "$name" "$dir" "$start" "$compat" <<'EOF'
import json, sys
name, directory, start, compat = sys.argv[1:]
print(json.dumps({
    "gameid": name, "directory": directory,
    "argv": [start], "env": {},
    "settings": {"steam_play": "0", "compat_tool": compat},
    "force_appid": "", "lepton_args": "",
}))
EOF
)
    res=$(python3 "$u/steam-client-create-shortcut" --parms "$parms" | tail -1)
    echo "Steam: $res"
    case $res in *'"error"'*) exit 1 ;; esac
    echo "registered $name ($dir)"
    ;;
  run)
    [ $# -eq 1 ] || usage
    python3 "$(utils)/steam-devkit-rpc" run-game "gameid=$1" | tail -1
    echo
    ;;
  remove)
    [ $# -eq 1 ] || usage
    python3 "$(utils)/steamos-delete" --delete-title "$1"
    rm -f "$HOME/devkit-game/$1"-*.json
    ;;
  list)
    python3 "$(utils)/steam-devkit-rpc" list-shortcuts
    ;;
  *) usage ;;
esac
