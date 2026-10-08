# Frametop as an image

The Frame's runtime is already a container: the dev container (Fedora toolbox
in distrobox) is where everything builds and runs, and `setup/dev-container.sh`
is its recipe. The recipe is the weak point — nothing in it is pinned. Rebuild
the container tomorrow and you get a different Fedora, a different wlroots, a
different glibc. This directory turns the container from a recipe into an
**artifact**: an OCI image whose every input is frozen.

## What the image contains

`Containerfile` builds one image, `frametop`, from three frozen inputs:

1. **The base image by digest** — `fedora-toolbox:44` pinned to the exact
   image, not the floating tag. This freezes the C world: glibc, wlroots 0.20,
   the Qt/KDE stack.
2. **Python from `uv.lock`** — the committed lockfile pins every Python
   dependency (pytest, ruff, the tracker's NumPy/OpenCV stack) to exact
   versions with aarch64 wheels, installed into `/opt/frametop/venv`. The venv
   sits on Fedora's own `python3`, which the base digest freezes, and sees the
   system site-packages: PySide6 and Kirigami come from dnf for that
   interpreter, so one `python3` imports both, as in the dev container.
3. **OpenVR from the pinned public tag** — `v2.15.6`, in
   `scripts/openvr.sh`. The build scripts fetch its headers and check their
   sha256. The image has no SteamVR, so it links the SDK's prebuilt
   `libopenvr_api.so` (in `/opt/frametop/lib`, also checked); the binaries'
   rpath names SteamVR's folder first, so on the Frame they load SteamVR's
   own library, as the on-device builds do.

The native binaries (`ft-screens`, `ft-pointer`, the `ft_pointer` driver,
`ft-gaze`, `ft-gazepanel`, `ft-powerd`) are built by the components' own
`build.sh` scripts, run inside the image with `FRAME_IN_BOX=1`, so there is
one build recipe for the image and the Frame. They stay in their `build/`
folders under `/src/frametop` and are copied to `/opt/frametop`. Hand
tracking stays deferred exactly as in `install.sh` (ncnn is a heavy,
separately triggered build).

What deliberately stays outside the image for now: the **host payload** — the
SteamVR driver registration (`vrpathreg`), ft-camd's file capabilities
(setcap), systemd units, the KWin script. Those need the SteamOS host, and
they are what `install.sh` does today. Packaging them into a checksummed
payload tarball is the next slice.

## The `ft` wrapper

Everything that runs Frametop goes through the wrapper at the repo root, so
the integration points live in one place:

```
ft dev build        # build the image from this repo (docker or podman)
ft update           # pull the published (versioned) image, pin its digest
ft dev shell        # interactive shell, repo at /src/frametop
ft ft-screens ...   # run any program from /opt/frametop
ft dev test         # the Python test suites inside the image
ft clean            # remove frametop's leftovers only (see the store section)
```

`FT_IMAGE` overrides the image reference (repo mode defaults to the locally
built `frametop:local`). Development commands live behind `ft dev` so an
installed copy — which has no repo to build from — refuses them. Containers
run through the wrapper are named `frametop-<program>-<pid>`. Runs get no
access to the host beyond the repo mount: how Frametop's programs run on the
Frame is still open (see [design.md](design.md)).

### No :latest on a headset

A moving tag has no place on a headset: it cannot be reproduced in a bug
report and cannot be rolled back. The wrapper enforces this for what gets
**deployed** — installed mode refuses to run without a pinned reference, and
both the update source and the pinned reference are rejected if they say
`:latest`. Local development can use any tag it likes (`FT_IMAGE` is never
second-guessed in repo mode).

How pinning works:

- `install.sh` (next slice) writes a **version tag** to
  `~/.config/frametop/published` — the only place updates come from.
- `ft update` pulls that reference and then writes the **digest**
  (`repo@sha256:...`) to `~/.config/frametop/image`. Every later run, and
  every bug report, names exactly that image.
- Rollback is editing one file: put the previous digest back into
  `~/.config/frametop/image`. The old image is still in the local store
  (remove it with `ft clean` when you are done with it).

Releases (below) pin the same way, by digest, but through the release list
and each release's `.frametop-release`, not `~/.config/frametop/published`
and `image`, and roll back by running the previous release's `install.sh`.
The two should become one before this ships.

## The shared podman store

On SteamOS, rootless podman has **one container/image store**, and Frametop
shares it with Valve's Android layer: Lepton names its containers
`lepton-<context>`, and they live in the same store as ours. Two rules
follow, and both are enforced or documented rather than hoped for:

1. **Frametop never touches anything it does not own.** Containers get stable
   `frametop-<program>` names; `ft clean` deletes only containers matching
   `frametop-*` and only images whose reference names frametop. It never runs
   store-wide commands.
2. **Nobody should run store-wide podman commands on a Frame.**
   `podman rm -a`, `podman rmi -a`, and `podman system prune` would delete
   Valve's containers and images too. Use `ft clean` for Frametop's share of
   the store and leave the rest of it to Steam.

## Network path for pulls

`ft update` (and install) contact the registry over the Frame's normal
Wi-Fi client interface (`wlan0`), not the 6 GHz streaming link to the PC
dongle. The image is roughly 1–1.5 GB compressed — pulls ride the home
network, so a weak Wi-Fi link is the bottleneck, not the streaming antenna.

## Storage

The image replaces the build toolchain, not adds to it: today's distrobox dev
container with its dnf history is the heavyweight; the pulled image carries
only the runtime plus the frozen build inputs (~1–1.5 GB compressed, a few GB
unpacked, in `~/.local/share/containers`). Once install.sh switches to the
image, the dev container is only needed for development and can be removed
from user machines. `ft clean` reclaims space from superseded frametop
images.

## Running on the Frame

The programs need much more of the host than a plain `podman run` gives them;
[design.md](design.md#the-runtime-on-the-frame) lists what, the device
findings so far, and the options. What's built so far is option A, a
distrobox made from the image, the same kind of container as the dev
container. The headset trial (step 2 below) decides.

Every program an installed Frametop runs in its container goes through
`scripts/in-box`: the pointer and power services, the gaze service's ft-gaze,
ft-eyes and calibration panel, the desktop's ft-screens, the remote desktop,
and the settings apps. A source install runs them in `dev`; a release names
its own container in `.frametop-release`.

## Releases

`get.sh --release` installs a release instead of cloning the repo:

1. It reads a release list (`frametop.release/v1`, written by
   `release-manifest.py`): `releases/stable.json` or `experimental.json` on
   Frametop's page, or `--manifest`. Each release names its version, commit,
   and image by digest.
2. It picks a release for this SteamOS build (`BUILD_ID` in
   `/etc/os-release`) from the SteamOS table, `steamos.json`, which every
   release in the list carries: the newest release tested on this build; else
   the newest not known to break on it, after a warning; and none if every
   release breaks on it (`--any-steamos` overrides). A broken build names the
   release that fixes it (`fixed_in`), or the first one that needs something
   it lacks (`from`), so an older SteamOS keeps getting the last release that
   works there.
3. It pulls the image by digest, copies its `/src/frametop` (the repo at that
   commit, built) to `~/.local/share/frametop/releases/VERSION`, writes
   `.frametop-release` there (version, image, commit, channel, and the
   container's name, `frametop-` and the digest's start), and runs that
   copy's `install.sh`.
4. In a release, `install.sh` builds nothing: it installs the distrobox the
   image brings, makes the release's container from the image
   (`release-box.sh`), and installs the services from the release's folder.
   Each release has its own container, so installing one doesn't stop the one
   running.
5. The release installed before stays; running its `install.sh` goes back to
   it. Older ones are removed, with their containers and images.
   `uninstall.sh` removes them all.

The FrameDrop download carries a release list too
([../framedrop/README.md](../framedrop/README.md)). Nothing publishes a release
list yet: that's step 3 below.

## CI

`.github/workflows/image.yml` builds the image on a native arm64 runner on
every push that touches it (and on `v*` tags), then, inside the built image,
runs the test gate (`just test-python test-c test-bash`) and a smoke test of
the image and the `ft` wrapper. Only `main` and `v*` tags push the image to
`ghcr.io/<owner>/frametop` (lowercase) and publish the wrapper with its
checksums; other branches are built and checked, not published. A new GHCR
package may start out private: check its visibility in the package settings
after the first push.

## The longer arc

The image only pays off when it makes installing Frametop easier, so none of
this ships until the whole path works:

1. Image, wrapper, and CI (this directory).
2. A headset trial: Frametop's services run from the image (the runtime
   question above), next to an install time measured against today's
   on-device build. Go or no-go here.
3. A release pipeline: one tag (and each green experimental commit) builds
   the image, pushes it, and publishes the release list with
   `release-manifest.py`. The image carries the host side too (its
   `/src/frametop`), so there's no separate tarball: one digest is the whole
   release.
4. `get.sh --release` and the FrameDrop package install a release (built:
   see Releases above). The source install stays for development.
