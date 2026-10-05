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
   versions with aarch64 wheels, installed into `/opt/frametop/venv` with its
   own uv-managed CPython. The venv is deliberately *not*
   system-site-packages: it must not depend on which Fedora is current.
3. **OpenVR from the pinned public tag** — the same `v2.15.6` the Frame-side
   build scripts pin; Valve no longer publishes release archives, so
   `libopenvr_api.so` is built from source in the image and lives in
   `/opt/frametop/lib`, found through the binaries' rpath.

The native binaries (`ft-screens`, `ft-pointer`, the `ft_pointer` driver,
`ft-gaze`, `ft-gazepanel`, `ft-powerd`) are compiled inside the image, the
same way the Frame-side `build.sh` scripts compile them. Hand tracking stays
deferred exactly as in `install.sh` (ncnn is a heavy, separately triggered
build).

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
run through the wrapper get stable names, `frametop-<program>`. The design
rationale is in [design.md](design.md).

### Never :latest

A moving tag has no place on a headset: it cannot be reproduced in a bug
report and cannot be rolled back. The wrapper enforces this — installed mode
refuses to run without a pinned reference, and both the update source and the
pinned reference are rejected if they say `:latest`.

How pinning works:

- `install.sh` (next slice) writes a **version tag** to
  `~/.config/frametop/published` — the only place updates come from.
- `ft update` pulls that reference and then writes the **digest**
  (`repo@sha256:...`) to `~/.config/frametop/image`. Every later run, and
  every bug report, names exactly that image.
- Rollback is editing one file: put the previous digest back into
  `~/.config/frametop/image`. The old image is still in the local store
  (remove it with `ft clean` when you are done with it).

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
`FT_FRAME=1` adds `--network=host` to *runs* (for sockets), never to the
pull.

## Storage

The image replaces the build toolchain, not adds to it: today's distrobox dev
container with its dnf history is the heavyweight; the pulled image carries
only the runtime plus the frozen build inputs (~1–1.5 GB compressed, a few GB
unpacked, in `~/.local/share/containers`). Once install.sh switches to the
image, the dev container is only needed for development and can be removed
from user machines. `ft clean` reclaims space from superseded frametop
images.

## Frame integration (designed, to be validated on the device)

The runtime model on the Frame: **podman, not distrobox**. Distrobox remains
for development; the product runs as a plain podman container started by the
systemd units. The wrapper carries the Frame's mounts, gated behind
`FT_FRAME=1` until each one is validated on the device:

- `--ipc=host` — the hands/gaze programs share results through files in
  `/dev/shm`; a container's private IPC namespace would hide them.
- `/run/user/1000` bind-mounted — the Wayland socket and Frametop's own
  `frametop-*` sockets live there.
- `/opt/steamvr` read-only — the real runtime (SteamVR's own `libopenvr_api`,
  `vrpathreg` for driver registration). The image's own API library keeps the
  image usable without SteamVR; the mount lets the Frame use the real one.

First device tests, in order: `ft ft-powerd` (talks to OpenVR only), then
`ft ft-screens` in a session, then the units. The mount matrix above is
expected to need adjusting there — that is what the gate is for.

## CI

`.github/workflows/image.yml` builds the image on a native arm64 runner on
every push that touches it (and on `v*` tags), runs the Python suites inside
the built image as a smoke test, and pushes to
`ghcr.io/0x1f6/frametop`. The fork's GHCR package starts out private — flip
it to public in the package settings after the first successful run (GitHub
web UI → your profile → Packages → `frametop` → Package settings →
"Change visibility"; there is no API for this). Pulling an image needs no
token once it is public; managing the package over `gh` needs
`gh auth refresh -s read:packages,write:packages`.

## The longer arc

1. ✅ Image + wrapper + CI (this directory)
2. Frame validation of the mount matrix, then `install.sh` puts the wrapper at
   `~/.local/bin/ft` (the SteamOS root is read-only, so no /usr/local) and the
   units switch their `ExecStart=` lines to it; the installed wrapper runs the
   image's own copy of the sources — no repo on the device needed
3. Host payload tarball (driver registration, units, KWin script, models) with
   checksums, attached to releases
4. `get.sh` becomes an artifact installer (download, verify, install); the
   source path stays for development
