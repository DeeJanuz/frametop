#!/usr/bin/env bash
# Install the Frametop release in this folder: a GitHub release's Frametop.zip, unpacked by
# FrameDrop (into ~/devkit-game/Frametop), by hand, or by get.sh --release. Next to this script
# are frametop-image.tar (Frametop, built, as a container image) and frametop-release.json (its
# version, commit, the image file's sha256 and the image's ID, and the SteamOS table). Nothing
# builds on the headset, and nothing else is downloaded.
#
# 1. It checks this SteamOS build against the SteamOS table: Frametop's newest from GitHub
#    (pack/steamos.json on main) when it can get it, else the one in the release. Tested: on.
#    Not tested yet: it says so and asks (--yes goes on). Broken for this release: it stops,
#    and names the release that fixes it (--any-steamos goes on anyway).
# 2. It checks the image file's sha256, and loads it into podman as localhost/frametop:VERSION.
# 3. It copies the release's files out of the image to ~/.local/share/frametop/releases/VERSION
#    and runs their install.sh, which makes the release's container and builds nothing.
# 4. The release installed before stays: running its install.sh goes back to it. Older ones
#    are removed, with their containers and images.
#
# Usage: install-release.sh [--yes] [--any-steamos] [--unpack-only] [--dir DIR]
#                           [--no-eye-tracker] [--no-bluetooth | --bluetooth]
#   --unpack-only  stop after step 2 and the copy: don't run install.sh
#   --dir DIR      where the releases go (default ~/.local/share/frametop/releases)
# The rest go to install.sh. FRAMETOP_STEAMOS_TABLE names another table to fetch (a URL), or
# "none" to use only the release's. Exit status 3: the image file is damaged.
set -euo pipefail

here=$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)
TABLE_URL=${FRAMETOP_STEAMOS_TABLE:-https://raw.githubusercontent.com/Frametop/frametop/main/pack/steamos.json}

# check_release RELEASE_JSON TABLE_JSON BUILD_ID: is the release usable, and is it for this
# SteamOS build? Prints seven lines: status (tested, untested, or broken), version, commit,
# channel, the image file's sha256, the image's ID, and a note for the user.
check_release() {
  python3 - "$@" <<'EOF'
import json, re, sys

release_path, table_path, build = sys.argv[1:4]
def fail(msg):
    print(f"the release's frametop-release.json isn't usable: {msg}", file=sys.stderr)
    sys.exit(1)
try:
    with open(release_path) as f:
        r = json.load(f)
except (OSError, ValueError) as e:
    fail(str(e))
if not isinstance(r, dict) or r.get("schema") != "frametop.release/v1":
    fail("not frametop.release/v1")
image = r.get("image") if isinstance(r.get("image"), dict) else {}
fields = {
    "version": (r.get("version"), r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}"),
    "commit": (r.get("commit"), r"[0-9a-f]{7,40}"),
    "channel": (r.get("channel", ""), r"(stable|experimental|)"),
    "image sha256": (image.get("sha256"), r"[0-9a-f]{64}"),
    "image id": (image.get("id"), r"[0-9a-f]{64}"),
}
for name, (value, pattern) in fields.items():
    if not isinstance(value, str) or not re.fullmatch(pattern, value):
        fail(f"bad {name}: {str(value)[:80]!r}")
version = r["version"]

def base(v):
    """0.3.0-exp.1 -> (0, 3, 0): the release it leads up to."""
    return tuple(int(p) for p in re.findall(r"\d+", str(v).split("-")[0].split("+")[0]))

table = r.get("steamos")
if table_path:  # Frametop's newest table, when it could be fetched
    try:
        with open(table_path) as f:
            newer = json.load(f)
        if isinstance(newer, dict) and isinstance(newer.get("tested"), list):
            table = newer
    except (OSError, ValueError):
        pass
table = table if isinstance(table, dict) else {}

def entries(kind):
    listed = table.get(kind)
    return [e for e in listed if isinstance(e, dict)] if isinstance(listed, list) else []

def oneline(s):
    return " ".join(str(s).split())[:300]

broken = [b for b in entries("broken") if b.get("build") == build
          and (not b.get("from") or base(version) >= base(b["from"]))
          and (not b.get("fixed_in") or base(version) < base(b["fixed_in"]))]
tested = [t for t in entries("tested") if t.get("build") == build]
if broken:
    status = "broken"
    note = oneline(broken[0].get("reason", "it doesn't work on this SteamOS build"))
    if broken[0].get("fixed_in"):
        note += f" (fixed in Frametop {oneline(broken[0]['fixed_in'])})"
elif tested:
    status, note = "tested", ""
else:
    status = "untested"
    others = entries("tested")[-3:]
    note = ("tested on " + ", ".join(f"SteamOS {e.get('version', '?')} (build {e.get('build', '?')})"
                                     for e in others)) if others else "not tested on any SteamOS build yet"
for line in (status, version, r["commit"], r["channel"], image["sha256"], image["id"], oneline(note)):
    print(line)
EOF
}

main() {
  local yes=0 any=0 unpack_only=0 base=$HOME/.local/share/frametop/releases tty=0 answer
  local pass=()
  while [ $# -gt 0 ]; do
    case $1 in
      --yes) yes=1; pass+=("$1") ;;
      --any-steamos) any=1 ;;
      --unpack-only) unpack_only=1 ;;
      --dir) base=${2:?--dir needs a folder}; shift ;;
      --no-eye-tracker|--no-bluetooth|--bluetooth) pass+=("$1") ;;
      -h|--help) sed -n '2,23p' "$0"; return 0 ;;
      *) echo "unknown option: $1" >&2; return 2 ;;
    esac
    shift
  done

  if ! { grep -qx 'ID=steamos' /etc/os-release && grep -qE '^VARIANT_ID="?vr"?$' /etc/os-release; } 2>/dev/null; then
    echo "Frametop installs on a Steam Frame (SteamOS, VR variant)." >&2
    return 1
  fi
  for f in frametop-release.json frametop-image.tar; do
    [ -f "$here/$f" ] || { echo "$here/$f is missing: unpack the whole Frametop.zip" >&2; return 1; }
  done
  { : </dev/tty; } 2>/dev/null && tty=1
  # podman's, even from a terminal in a VR desktop (its session has its own)
  export XDG_RUNTIME_DIR=/run/user/$(id -u)
  export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus

  local build osver tmp status version commit channel sha id note
  build=$(sed -n 's/^BUILD_ID=//p' /etc/os-release | tr -d '"')
  osver=$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d '"')
  tmp=$(mktemp)
  if [ "$TABLE_URL" = none ] || ! curl -fsS --max-time 8 --proto '=https' "$TABLE_URL" -o "$tmp" 2>/dev/null; then
    : >"$tmp"  # offline: the release's own table
  fi
  { read -r status; read -r version; read -r commit; read -r channel; read -r sha; read -r id; read -r note; } \
    < <(check_release "$here/frametop-release.json" "$([ -s "$tmp" ] && echo "$tmp")" "$build") ||
    { rm -f "$tmp"; return 1; }
  rm -f "$tmp"
  [ -n "${id:-}" ] || return 1

  case $status in
    tested) echo "Frametop $version: tested on this SteamOS ($osver, build $build)" ;;
    untested)
      echo "Frametop $version hasn't been tested on this SteamOS yet ($osver, build $build); $note."
      echo "It usually works: SteamOS updates rarely change what Frametop uses. If something's"
      echo "wrong afterwards, scripts/doctor.sh in the release's folder says what changed."
      if [ "$yes" = 0 ]; then
        [ "$tty" = 1 ] || { echo "No terminal to ask in: add --yes to install it anyway." >&2; return 1; }
        read -r -p "Install it anyway? [Y/n] " answer </dev/tty || answer=
        [[ ${answer:-y} =~ ^[Yy] ]] || return 1
      fi ;;
    broken)
      echo "Frametop $version doesn't work on this SteamOS ($osver, build $build): $note." >&2
      if [ "$any" = 0 ]; then
        echo "Nothing installed. --any-steamos installs it anyway." >&2
        return 1
      fi ;;
  esac

  local image=localhost/frametop:$version box=frametop-${id:0:12} dest=$base/$version
  if [ -f "$dest/.frametop-release" ] && ! grep -qxF "IMAGE_ID=$id" "$dest/.frametop-release"; then
    echo "$dest holds another build of Frametop $version. Move it away, then run this again." >&2
    return 1
  fi

  printf '\n\033[1m== 0/10 loading Frametop %s (a minute or two)\033[0m\n' "$version"
  if [ "$(podman image inspect -f '{{.Id}}' "$image" 2>/dev/null)" = "$id" ]; then
    echo "already loaded"
  else
    local free
    free=$(df -P -BG "$HOME" | awk 'NR == 2 { sub("G", "", $4); print $4 }')
    if [ "${free:-0}" -lt 5 ]; then
      echo "Only ${free:-0} GB free in your home folder; Frametop needs about 4 GB. Free some space first." >&2
      return 1
    fi
    echo "checking the image file"
    echo "$sha  $here/frametop-image.tar" | sha256sum -c --status ||
      { echo "frametop-image.tar is damaged (its sha256 doesn't match): download Frametop.zip again" >&2; return 3; }
    podman load -q -i "$here/frametop-image.tar" >/dev/null
    podman image exists "$id" ||
      { echo "the image file didn't load as the image frametop-release.json names" >&2; return 1; }
    podman tag "$id" "$image"
  fi

  if [ ! -f "$dest/.frametop-release" ]; then
    echo "copying the release's files to $dest"
    mkdir -p "$base"
    rm -rf "$dest.new"
    mkdir "$dest.new"
    local copied=0
    podman rm -f "frametop-copy-$$" >/dev/null 2>&1 || true
    podman create --name "frametop-copy-$$" "$image" true >/dev/null &&
      podman cp "frametop-copy-$$:/src/frametop/." "$dest.new/" || copied=1
    podman rm -f "frametop-copy-$$" >/dev/null 2>&1 || true
    [ "$copied" = 0 ] || { rm -rf "$dest.new"; echo "couldn't copy the release's files out of its image" >&2; return 1; }
    printf 'VERSION=%s\nCOMMIT=%s\nCHANNEL=%s\nIMAGE=%s\nIMAGE_ID=%s\nBOX=%s\n' \
      "$version" "$commit" "$channel" "$image" "$id" "$box" >"$dest.new/.frametop-release"
    mv "$dest.new" "$dest"
  fi
  echo "Frametop $version (${commit:0:7}) in $dest"

  if [ "$unpack_only" = 1 ]; then
    echo "Install with: $dest/install.sh"
    return 0
  fi
  if [ "$tty" = 1 ]; then
    "$dest/install.sh" ${pass[@]+"${pass[@]}"} </dev/tty
  else
    "$dest/install.sh" ${pass[@]+"${pass[@]}"} </dev/null
  fi

  # This release is now "current"; the one before stays, to go back to. Older ones go, with
  # their containers and images (only Frametop's: frametop-... and localhost/frametop:...).
  local prev= d old_image old_box
  [ -L "$base/current" ] && prev=$(readlink "$base/current")
  ln -sfn "$version" "$base/current"
  for d in "$base"/*/; do
    d=${d%/}
    [ -L "$d" ] && continue  # "current" itself, a link to the release just installed
    [ -f "$d/.frametop-release" ] || continue
    case ${d##*/} in "$version"|"$prev") continue ;; esac
    old_image=$(sed -n 's/^IMAGE=//p' "$d/.frametop-release")
    old_box=$(sed -n 's/^BOX=//p' "$d/.frametop-release")
    echo "removing Frametop ${d##*/}, two releases back"
    if [[ $old_box =~ ^frametop-[A-Za-z0-9_.-]+$ ]]; then podman rm -f "$old_box" >/dev/null 2>&1 || true; fi
    if [[ $old_image == localhost/frametop:* ]]; then podman rmi "$old_image" >/dev/null 2>&1 || true; fi
    rm -rf "$d"
  done
}

main "$@"
