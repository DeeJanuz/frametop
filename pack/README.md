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

Releases (below) don't use these: a release is a file, its image is pinned
by ID in `.frametop-release`, and you roll back by running the previous
release's `install.sh`.

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

A release is one file, **Frametop.zip** (about 1.1 GB), attached to a GitHub
release: the image as an OCI archive (`frametop-image.tar`, from `podman save`),
`frametop-release.json` (version, commit, the archive's sha256, the image's
ID, and the SteamOS table), the installer `install-release.sh`, and the
install window from `framedrop/installer`. No container registry. It installs
three ways, all through the same installer:

- FrameDrop on a PC copies the zip's folder to the headset and adds "Frametop"
  to the library; Play opens the install window (`framedrop/README.md`).
- Unpacked on the headset, `Frametop/frametop-install.sh` opens the same
  window.
- `get.sh --release` downloads the zip from GitHub (the newest stable release,
  or `--experimental`, `--version V`, `--zip FILE`) and runs
  `install-release.sh` in the terminal.

`install-release.sh`:

1. Checks this SteamOS build (`BUILD_ID` in `/etc/os-release`) against the
   SteamOS table, `steamos.json`: the newest one, from main on GitHub, when it
   can fetch it, else the copy in the release. Tested: on. Not tested yet: a
   warning, and a question (`--yes` goes on). Broken for this release: it stops
   and names the release that fixes it (`--any-steamos` goes on). A broken build
   counts from its `from` release up to its `fixed_in`, so a release that needs
   a newer SteamOS refuses an older one, and the other way round.
2. Checks the archive's sha256 (a damaged copy stops it, exit status 3, and
   `get.sh` downloads again), loads it into podman, checks the image's ID, and
   tags it `localhost/frametop:VERSION`.
3. Copies the image's `/src/frametop` (the repo at that commit, built) to
   `~/.local/share/frametop/releases/VERSION` with a `.frametop-release`
   (version, commit, channel, image, ID, and the container's name, `frametop-`
   and the ID's start), and runs that copy's `install.sh`.
4. In a release, `install.sh` builds nothing: it installs the distrobox the
   image brings, makes the release's container from the image
   (`release-box.sh`, which checks the ID again), and installs the services
   from the release's folder. Each release has its own container, so
   installing one doesn't stop the one running.
5. The release installed before stays; running its `install.sh` goes back to
   it. Older ones are removed, with their containers and images.
   `uninstall.sh` removes them all.

Every update is a full 1.1 GB download, since the zip holds the whole image
(user decision, 2026-10-07: one file, no registry). The `ft` wrapper's
installed mode (`ft update`, `~/.config/frametop/published` and `image`) pulls
from a registry, so releases don't use it.

## CI

`.github/workflows/release.yml` runs on Depot's arm64 runners
(`depot-ubuntu-24.04-arm-4`), only for a `v*` tag or by hand: no pushes or pull
requests, since the runners are paid and a fork's pull request would run on
them. It builds the image with podman (`ft dev build`, as on the Frame), runs
the test gate inside it (`ft dev test`: Python, C, and shell), checks what a
release installs from the image, and builds Frametop.zip with
`framedrop/build.sh --image`. A tag makes a draft GitHub release with the zip,
its FrameDrop manifest, `frametop-release.json`, and `SHA256SUMS`, as a
prerelease when the tag has a `-` (`v0.3.0-exp.1`); someone publishes it. A
manual run keeps the zip as an artifact for a week. The repo needs Depot's
GitHub app for the runner label to work.

Two repos (2026-10-09): Frametop/frametop, the organization's, is the home.
Its CI builds the releases (Depot's runners need an organization), its Pages
(`frametop.github.io/frametop`, from main) serve `get.sh` and `uninstall.sh`,
and new clones come from it. DeeJanuz/frametop is the upstream it mirrors:
issues and pull requests go there, and older clones update from it. A release:
merge into experimental (and main for a stable one, by merge commit), push both
branches to both repos, then tag on Frametop/frametop and publish the draft.
DeeJanuz/frametop's Pages serve a `gh-pages` branch whose `get.sh` and
`uninstall.sh` only hand over to the organization's, so the one-liner from
before keeps working.

## The longer arc

The image only pays off when it makes installing Frametop easier, so none of
this ships until the whole path works:

1. Image, wrapper, and CI (this directory).
2. A headset trial: Frametop's services run from the image (the runtime
   question above), next to an install time measured against today's
   on-device build. Go or no-go here.
3. A release pipeline: a tag builds the image, tests it, and attaches
   Frametop.zip to a draft GitHub release (CI above). The image carries the
   host side too (its `/src/frametop`), so there's no separate tarball.
4. FrameDrop, the unpacked zip, and `get.sh --release` install a release
   (Releases above). The source install stays for development.
