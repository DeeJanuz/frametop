#!/usr/bin/env bash
# Frametop's one-line installer. In a terminal on the Steam Frame (Konsole in the desktop, or
# over SSH):
#
#   curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash
#
# It asks which version to install, clones the repo into ~/frametop (or updates the clone
# that's there), and runs its install.sh. Run it again to update, or to switch versions.
# With --release it installs a release instead: Frametop built, in an image (pack/README.md).
# It reads the release list (a manifest), picks the newest release tested on this SteamOS
# build, pulls its image by digest, copies the release's files out of it into
# ~/.local/share/frametop/releases/VERSION, and runs that copy's install.sh. Nothing builds on
# the headset. The release installed before stays, so running its install.sh goes back to it.
# Options (piped, they go after "bash -s --"):
#   --stable        the main branch: tested releases (the default for a new install)
#   --experimental  the experimental branch: the newest features, less tested
#   --branch NAME   another branch, such as a fix to test before it's released
#   --dir DIR       where the repo goes (default ~/frametop; for --release, where the
#                   releases go, default ~/.local/share/frametop/releases)
#   --clone-only    get or update the repo (or the release), but don't run install.sh
#   --release       install a release, from the stable or experimental list
#   --manifest FILE|URL   with --release: the release list to use instead
#   --version V     with --release: this release from the list, not the newest that fits
#   --any-steamos   with --release: install even on a SteamOS build the release lists as broken
#   --yes, --no-bluetooth, --bluetooth   passed to install.sh (--yes also answers this
#                   script's questions: the version already there, or stable)
set -euo pipefail

usage() {
  cat <<'EOF'
usage: get.sh [--stable | --experimental | --branch NAME] [--dir DIR] [--clone-only] [--yes]
              [--no-bluetooth | --bluetooth]
       get.sh --release [--stable | --experimental | --manifest FILE|URL] [--version V]
              [--any-steamos] [--dir DIR] [--clone-only] [--yes] [--no-bluetooth | --bluetooth]
piped: curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash -s -- [options]
EOF
}

# The release lists: releases/stable.json and releases/experimental.json on Frametop's page.
# FRAMETOP_RELEASES points somewhere else, for testing; FRAMETOP_TLS_VERIFY=false pulls from a
# registry without TLS, such as a test registry on this machine.
RELEASES=${FRAMETOP_RELEASES:-https://deejanuz.github.io/frametop/releases}

# release_pick MANIFEST BUILD_ID VERSION: check the release list and pick a release for this
# SteamOS build. Prints six lines: status (tested, untested, or broken), version, image,
# commit, channel, and a note for the user.
release_pick() {
  python3 - "$@" <<'EOF'
import json, re, sys

path, build, want = sys.argv[1:4]
def fail(msg):
    print(f"the release list isn't usable: {msg}", file=sys.stderr)
    sys.exit(1)
try:
    with open(path) as f:
        m = json.load(f)
except (OSError, ValueError) as e:
    fail(str(e))
if not isinstance(m, dict) or m.get("schema") != "frametop.release/v1":
    fail("not a frametop.release/v1 list")
rels = m.get("releases")
if not isinstance(rels, list) or not rels:
    fail("no releases in it")
channel = str(m.get("channel", ""))
if not re.fullmatch(r"[a-z0-9-]{0,32}", channel):
    fail("bad channel")
for r in rels:
    ok = (isinstance(r, dict)
          and re.fullmatch(r"[0-9A-Za-z][0-9A-Za-z.+_-]{0,63}", str(r.get("version", "")))
          and re.fullmatch(r"[a-z0-9][a-z0-9._/:-]{0,200}@sha256:[0-9a-f]{64}", str(r.get("image", "")))
          and re.fullmatch(r"[0-9a-f]{7,40}", str(r.get("commit", ""))))
    if not ok:
        fail(f"a release with a bad version, image, or commit: {str(r)[:120]}")
if want:
    rels = [r for r in rels if r["version"] == want]
    if not rels:
        fail(f"no release {want} in it")

def entries(r, kind):
    os_ = r.get("steamos")
    listed = os_.get(kind) if isinstance(os_, dict) else None
    return {e.get("build"): e for e in listed if isinstance(e, dict)} if isinstance(listed, list) else {}

def oneline(s):
    return " ".join(str(s).split())[:300]

def named(e):
    return f"SteamOS {e.get('version', '?')} (build {e.get('build', '?')})"

pick, status, note = None, "broken", ""
for r in rels:  # newest first: the newest release tested on this build (and not broken on it)
    if build in entries(r, "tested") and build not in entries(r, "broken"):
        pick, status = r, "tested"
        break
else:
    for r in rels:  # else the newest one that isn't known to break on it
        if build not in entries(r, "broken"):
            pick, status = r, "untested"
            tested = list(entries(r, "tested").values())
            note = ("tested on " + ", ".join(named(e) for e in tested[-3:])) if tested else "not tested on any SteamOS build yet"
            break
    else:
        pick = rels[0]
        e = entries(pick, "broken")[build]
        note = oneline(e.get("reason", "it doesn't work on this SteamOS build"))
        if e.get("fixed_in"):
            note += f" (fixed in Frametop {oneline(e['fixed_in'])})"
for line in (status, pick["version"], pick["image"], pick["commit"], channel, oneline(note)):
    print(line)
EOF
}

# install_release CHANNEL MANIFEST DIR VERSION CLONE_ONLY YES TTY ANY_STEAMOS -- INSTALL_ARGS...
install_release() {
  local channel=$1 manifest=$2 base=$3 want=$4 clone_only=$5 yes=$6 tty=$7 any=$8
  shift 9
  local tmp build osver status version image commit note digest dest box answer prev= free
  local tls=()
  [ "${FRAMETOP_TLS_VERIFY:-true}" = false ] && tls=(--tls-verify=false)
  for tool in podman python3 curl; do
    command -v "$tool" >/dev/null || { echo "$tool isn't here: a release needs it" >&2; return 1; }
  done
  export XDG_RUNTIME_DIR=/run/user/$(id -u)  # podman's, even from a terminal in a VR desktop
  tmp=$(mktemp -d)
  [ -n "$manifest" ] || manifest=$RELEASES/$channel.json
  case $manifest in
    https://*) curl -fsSL --proto '=https' "$manifest" -o "$tmp/releases.json" ||
                 { rm -rf "$tmp"; echo "couldn't download the release list: $manifest" >&2; return 1; } ;;
    *://*) rm -rf "$tmp"; echo "the release list has to come over https: $manifest" >&2; return 1 ;;
    *) cp "$manifest" "$tmp/releases.json" || { rm -rf "$tmp"; return 1; } ;;
  esac

  build=$(sed -n 's/^BUILD_ID=//p' /etc/os-release | tr -d '"')
  osver=$(sed -n 's/^VERSION_ID=//p' /etc/os-release | tr -d '"')
  { read -r status; read -r version; read -r image; read -r commit; read -r channel; read -r note; } \
    < <(release_pick "$tmp/releases.json" "$build" "$want") || { rm -rf "$tmp"; return 1; }
  rm -rf "$tmp"
  [ -n "$version" ] || return 1
  case $status in
    tested) echo "Frametop $version: tested on this SteamOS ($osver, build $build)" ;;
    untested)
      echo "Frametop $version hasn't been tested on this SteamOS yet ($osver, build $build); $note."
      echo "It usually works: SteamOS updates rarely change what Frametop uses. If something's"
      echo "wrong, scripts/doctor.sh in the release's folder says what changed."
      if [ "$yes" = 0 ]; then
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

  digest=${image##*@sha256:}
  box=frametop-${digest:0:12}
  dest=$base/$version
  if [ -f "$dest/.frametop-release" ] && ! grep -qxF "IMAGE=$image" "$dest/.frametop-release"; then
    echo "$dest holds another build of Frametop $version. Move it away, then run this again." >&2
    return 1
  fi
  free=$(df -P -BG "$HOME" | awk 'NR == 2 { sub("G", "", $4); print $4 }')
  if [ "${free:-0}" -lt 6 ] && ! podman image exists "$image"; then
    echo "Only ${free:-0} GB free in your home folder; the release needs about 4 GB. Free some space first." >&2
    return 1
  fi

  printf '\n\033[1m== 0/10 downloading Frametop %s (about 1.5 GB the first time; an update, less)\033[0m\n' "$version"
  podman pull ${tls[@]+"${tls[@]}"} "$image" || { echo "couldn't download $image" >&2; return 1; }

  if [ ! -f "$dest/.frametop-release" ]; then
    echo "copying the release's files to $dest"
    mkdir -p "$base"
    rm -rf "$dest.new"
    mkdir "$dest.new"
    podman rm -f "frametop-copy-$$" >/dev/null 2>&1 || true
    local copied=0
    podman create --name "frametop-copy-$$" "$image" true >/dev/null &&
      podman cp "frametop-copy-$$:/src/frametop/." "$dest.new/" || copied=1
    podman rm -f "frametop-copy-$$" >/dev/null 2>&1 || true
    [ "$copied" = 0 ] || { rm -rf "$dest.new"; echo "couldn't copy the release's files out of its image" >&2; return 1; }
    printf 'VERSION=%s\nIMAGE=%s\nCOMMIT=%s\nCHANNEL=%s\nBOX=%s\n' \
      "$version" "$image" "$commit" "$channel" "$box" > "$dest.new/.frametop-release"
    mv "$dest.new" "$dest"
  fi
  echo "Frametop $version ($commit) in $dest"

  if [ "$clone_only" = 1 ]; then
    echo "Install with: $dest/install.sh"
    return 0
  fi
  if [ "$tty" = 1 ]; then
    "$dest/install.sh" "$@" </dev/tty || return
  else
    "$dest/install.sh" "$@" </dev/null || return
  fi

  # The release now installed is "current"; the one before stays, to go back to (run its
  # install.sh). Older ones go, with their containers and images.
  [ -L "$base/current" ] && prev=$(readlink "$base/current")
  ln -sfn "$version" "$base/current"
  local d old_image old_box
  for d in "$base"/*/; do
    d=${d%/}
    [ -f "$d/.frametop-release" ] || continue
    case ${d##*/} in "$version"|"$prev") continue ;; esac
    old_image=$(sed -n 's/^IMAGE=//p' "$d/.frametop-release")
    old_box=$(sed -n 's/^BOX=//p' "$d/.frametop-release")
    echo "removing Frametop ${d##*/}, two releases back"
    if [[ $old_box =~ ^frametop-[A-Za-z0-9_.-]+$ ]]; then podman rm -f "$old_box" >/dev/null 2>&1 || true; fi
    if [[ $old_image == *frametop*@sha256:* ]]; then podman rmi "$old_image" >/dev/null 2>&1 || true; fi
    rm -rf "$d"
  done
}

# Everything happens in main, called on the last line, so a download cut short runs nothing.
main() {
  local repo=https://github.com/DeeJanuz/frametop.git dir= branch= clone_only=0
  local yes=0 tty=0 current= def answer release=0 manifest= want= any=0
  local pass=()
  while [ $# -gt 0 ]; do
    case $1 in
      --stable) branch=main ;;
      --experimental) branch=experimental ;;
      --branch) branch=${2:?--branch needs a branch name}; shift ;;
      --dir) dir=${2:?--dir needs a folder}; shift ;;
      --clone-only) clone_only=1 ;;
      --release) release=1 ;;
      --manifest) manifest=${2:?--manifest needs a file or URL}; shift ;;
      --version) want=${2:?--version needs a version}; shift ;;
      --any-steamos) any=1 ;;
      --yes) yes=1; pass+=("$1") ;;
      --no-bluetooth|--bluetooth) pass+=("$1") ;;
      -h|--help) usage; return 0 ;;
      *) echo "unknown option: $1" >&2; usage >&2; return 2 ;;
    esac
    shift
  done
  if [ "$release" = 0 ] && { [ -n "$manifest" ] || [ -n "$want" ] || [ "$any" = 1 ]; }; then
    echo "--manifest, --version, and --any-steamos go with --release" >&2
    return 2
  fi
  if [ "$release" = 1 ] && [ -n "$branch" ] && [ "$branch" != main ] && [ "$branch" != experimental ]; then
    echo "releases come from the stable or experimental list, not a branch" >&2
    return 2
  fi
  if [ -z "$dir" ]; then
    dir=$HOME/frametop
    [ "$release" = 1 ] && dir=$HOME/.local/share/frametop/releases
  fi

  if ! { grep -qx 'ID=steamos' /etc/os-release && grep -qE '^VARIANT_ID="?vr"?$' /etc/os-release; } 2>/dev/null; then
    echo "Frametop installs on a Steam Frame (SteamOS, VR variant). Run this in a terminal on the headset." >&2
    return 1
  fi
  # Piped into bash, stdin is this script: the questions (here and install.sh's) read the terminal.
  { : </dev/tty; } 2>/dev/null && tty=1
  if [ "$tty" = 0 ] && [ "$yes" = 0 ]; then
    echo "This asks questions, and there's no terminal to ask in: run it in one, or add --yes." >&2
    return 1
  fi

  if [ "$release" = 0 ] && [ -e "$dir/.git" ]; then
    git -C "$dir" remote get-url origin 2>/dev/null | grep -qi 'frametop' ||
      { echo "$dir is a git repo, but not Frametop's. Pick another folder with --dir." >&2; return 1; }
    current=$(git -C "$dir" branch --show-current)
  elif [ "$release" = 0 ] && [ -e "$dir" ]; then
    echo "$dir is there and isn't Frametop's repo. Move it, or pick another folder with --dir." >&2
    return 1
  elif [ "$release" = 1 ] && [ -f "$dir/current/.frametop-release" ]; then
    current=$(sed -n 's/^CHANNEL=//p' "$dir/current/.frametop-release")
    [ "$current" = stable ] && current=main
  fi

  if [ -z "$branch" ] && { [ "$release" = 0 ] || [ -z "$manifest" ]; }; then
    def=main
    [ "$current" = experimental ] && def=experimental
    if [ "$yes" = 1 ]; then
      branch=$def
    else
      echo "Which version of Frametop?"
      echo "  1) stable: the main branch, tested releases"
      echo "  2) experimental: the newest features, less tested"
      [ -n "$current" ] && echo "(installed now: $current)"
      read -r -p "Choose 1 or 2 [$([ "$def" = main ] && echo 1 || echo 2)]: " answer </dev/tty || answer=
      case ${answer:-$def} in
        1|main|s*) branch=main ;;
        2|experimental|e*) branch=experimental ;;
        *) echo "not 1 or 2: $answer" >&2; return 2 ;;
      esac
    fi
  fi

  if [ "$release" = 1 ]; then
    install_release "$([ "$branch" = experimental ] && echo experimental || echo stable)" "$manifest" "$dir" \
      "$want" "$clone_only" "$yes" "$tty" "$any" -- "${pass[@]}"
    return
  fi

  if ! git ls-remote --exit-code --heads "$repo" "$branch" >/dev/null; then
    echo "Frametop has no branch called $branch (or GitHub can't be reached)." >&2
    return 1
  fi

  if [ ! -e "$dir" ]; then
    echo "Cloning Frametop ($branch) into $dir"
    git clone --branch "$branch" "$repo" "$dir"
  else
    if ! git -C "$dir" diff --quiet || ! git -C "$dir" diff --cached --quiet; then
      echo "$dir has changes of its own. Commit or stash them first (git -C $dir status)." >&2
      return 1
    fi
    echo "Updating $dir to the latest $branch"
    git -C "$dir" fetch --quiet origin
    if [ "$current" != "$branch" ]; then
      if git -C "$dir" show-ref --verify --quiet "refs/heads/$branch"; then
        git -C "$dir" switch --quiet "$branch"
      else
        git -C "$dir" switch --quiet --track -c "$branch" "origin/$branch"
      fi
    fi
    git -C "$dir" merge --ff-only --quiet "origin/$branch" ||
      { echo "$dir has commits of its own on $branch, so it can't just move to the latest. Update it by hand." >&2; return 1; }
  fi
  echo "Frametop $branch: $(git -C "$dir" log -1 --format='%h %s')"

  if [ "$clone_only" = 1 ]; then
    echo "Install with: cd $dir && ./install.sh"
    return 0
  fi
  cd "$dir"
  if [ "$tty" = 1 ]; then
    ./install.sh "${pass[@]}" </dev/tty
  else
    ./install.sh "${pass[@]}" </dev/null
  fi
}

main "$@"
