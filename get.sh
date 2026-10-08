#!/usr/bin/env bash
# Frametop's one-line installer. In a terminal on the Steam Frame (Konsole in the desktop, or
# over SSH):
#
#   curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash
#
# It asks which version to install, clones the repo into ~/frametop (or updates the clone
# that's there), and runs its install.sh. Run it again to update, or to switch versions.
# With --release it installs a release instead: Frametop built, in one file. It downloads the
# release's Frametop.zip from GitHub (about 1.1 GB: the newest stable release, or with
# --experimental the newest of any), unpacks it in ~/.cache/frametop/release, and runs its
# install-release.sh, which checks this SteamOS build against the releases' SteamOS table and
# installs without building anything (pack/README.md, Releases). The same zip installs with
# FrameDrop from a PC, or unpacked by hand on the headset.
# Options (piped, they go after "bash -s --"):
#   --stable        the main branch: tested releases (the default for a new install)
#   --experimental  the experimental branch: the newest features, less tested
#   --branch NAME   another branch, such as a fix to test before it's released
#   --dir DIR       where the repo goes (default ~/frametop; for --release, where the
#                   releases go, default ~/.local/share/frametop/releases)
#   --clone-only    get or update the repo (or unpack the release), but don't run install.sh
#   --release       install a release from GitHub instead of cloning the repo
#   --version V     with --release: that release (tag vV), not the newest
#   --zip FILE|URL  with --release: this Frametop.zip instead of GitHub's newest
#   --any-steamos   with --release: install even on a SteamOS build the release breaks on
#   --yes, --no-eye-tracker, --no-bluetooth, --bluetooth   passed to install.sh (--yes also
#                   answers this script's questions: the version already there, or stable)
set -euo pipefail

usage() {
  cat <<'EOF'
usage: get.sh [--stable | --experimental | --branch NAME] [--dir DIR] [--clone-only] [--yes]
              [--no-eye-tracker] [--no-bluetooth | --bluetooth]
       get.sh --release [--stable | --experimental | --version V | --zip FILE|URL]
              [--any-steamos] [--dir DIR] [--clone-only] [--yes] [--no-eye-tracker]
              [--no-bluetooth | --bluetooth]
piped: curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash -s -- [options]
EOF
}

SLUG=${FRAMETOP_REPO:-DeeJanuz/frametop}  # FRAMETOP_REPO: another repo's releases, such as a fork's

# release_zip CHANNEL VERSION: the URL of a release's Frametop.zip on GitHub.
release_zip() {
  if [ -n "$2" ]; then
    echo "https://github.com/$SLUG/releases/download/v$2/Frametop.zip"
  elif [ "$1" = stable ]; then
    echo "https://github.com/$SLUG/releases/latest/download/Frametop.zip"  # newest non-prerelease
  else
    curl -fsSL "https://api.github.com/repos/$SLUG/releases?per_page=30" | python3 -c '
import json, sys
for r in json.load(sys.stdin):  # newest first
    for a in r.get("assets", []):
        if a.get("name") == "Frametop.zip" and not r.get("draft"):
            print(a["browser_download_url"])
            sys.exit(0)
sys.exit(1)'
  fi
}

# install_release CHANNEL VERSION ZIP DIR CLONE_ONLY ANY_STEAMOS -- INSTALL_ARGS...
install_release() {
  local channel=$1 version=$2 zip=$3 dir=$4 clone_only=$5 any=$6 cache=$HOME/.cache/frametop/release
  shift 7
  local args=("$@")
  [ -n "$dir" ] && args+=(--dir "$dir")
  [ "$clone_only" = 1 ] && args+=(--unpack-only)
  [ "$any" = 1 ] && args+=(--any-steamos)
  mkdir -p "$cache"
  local url= file loc key status=0
  case $zip in
    https://*|'')
      url=$zip
      if [ -z "$url" ]; then
        url=$(release_zip "$channel" "$version") ||
          { echo "couldn't find a Frametop release on GitHub (or GitHub can't be reached)" >&2; return 1; }
      fi
      # "latest" names a different file after each release: resume only the same release's.
      if [[ $url == */releases/latest/download/* ]]; then
        loc=$(curl -fsSI --proto '=https' "$url" | tr -d '\r' | sed -n 's/^[Ll]ocation: //p' | head -1)
        [[ $loc == https://github.com/* ]] && url=$loc
      fi
      key=$(printf %s "$url" | sha256sum | cut -c1-16)
      file=$cache/Frametop-$key.zip
      find "$cache" -maxdepth 1 -name 'Frametop-*.zip*' ! -name "Frametop-$key.zip*" -delete
      if [ ! -f "$file" ]; then
        echo "Downloading $url (about 1.1 GB; if it stops, run this again to resume)"
        curl -fL --proto '=https' -C - -o "$file.part" "$url" ||
          { echo "the download didn't finish: run this again to resume it" >&2; return 1; }
        mv "$file.part" "$file"
      fi ;;
    *://*) echo "the zip has to come over https: $zip" >&2; return 1 ;;
    *) file=$zip; [ -f "$file" ] || { echo "no file $file" >&2; return 1; } ;;
  esac
  echo "Unpacking $file"
  rm -rf "$cache/unpacked"
  if ! unzip -q "$file" -d "$cache/unpacked"; then
    [ -n "$url" ] && rm -f "$file"
    echo "$file didn't unpack: run this again to download it again" >&2
    return 1
  fi
  "$cache/unpacked/Frametop/install-release.sh" "${args[@]}" || status=$?
  rm -rf "$cache/unpacked"
  if [ "$status" = 3 ] && [ -n "$url" ]; then
    rm -f "$file"  # damaged: the next run downloads it again
  fi
  [ "$status" = 0 ] || return "$status"
  # Loaded into podman and copied out: the download isn't needed any more.
  [ "$clone_only" = 1 ] || rm -rf "$cache"
}

# Everything happens in main, called on the last line, so a download cut short runs nothing.
main() {
  local repo=https://github.com/DeeJanuz/frametop.git dir= branch= clone_only=0
  local yes=0 tty=0 current= def answer release=0 zip= want= any=0
  local pass=()
  while [ $# -gt 0 ]; do
    case $1 in
      --stable) branch=main ;;
      --experimental) branch=experimental ;;
      --branch) branch=${2:?--branch needs a branch name}; shift ;;
      --dir) dir=${2:?--dir needs a folder}; shift ;;
      --clone-only) clone_only=1 ;;
      --release) release=1 ;;
      --zip) zip=${2:?--zip needs a file or URL}; shift ;;
      --version) want=${2:?--version needs a version}; shift ;;
      --any-steamos) any=1 ;;
      --yes) yes=1; pass+=("$1") ;;
      --no-bluetooth|--bluetooth|--no-eye-tracker) pass+=("$1") ;;
      -h|--help) usage; return 0 ;;
      *) echo "unknown option: $1" >&2; usage >&2; return 2 ;;
    esac
    shift
  done
  if [ "$release" = 0 ] && { [ -n "$zip" ] || [ -n "$want" ] || [ "$any" = 1 ]; }; then
    echo "--zip, --version, and --any-steamos go with --release" >&2
    return 2
  fi
  if [ "$release" = 1 ] && [ -n "$branch" ] && [ "$branch" != main ] && [ "$branch" != experimental ]; then
    echo "releases come from the stable or experimental list, not a branch" >&2
    return 2
  fi
  if [ -z "$dir" ] && [ "$release" = 0 ]; then
    dir=$HOME/frametop
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
  elif [ "$release" = 1 ] && [ -f "${dir:-$HOME/.local/share/frametop/releases}/current/.frametop-release" ]; then
    current=$(sed -n 's/^CHANNEL=//p' "${dir:-$HOME/.local/share/frametop/releases}/current/.frametop-release")
    [ "$current" = stable ] && current=main
  fi

  if [ -z "$branch" ] && { [ "$release" = 0 ] || { [ -z "$zip" ] && [ -z "$want" ]; }; }; then
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
    install_release "$([ "$branch" = experimental ] && echo experimental || echo stable)" "$want" "$zip" "$dir" \
      "$clone_only" "$any" -- ${pass[@]+"${pass[@]}"}
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
