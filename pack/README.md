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
ft build            # build the image from this repo (docker or podman)
ft update           # pull the published image (ghcr.io/0x1f6/frametop) and retag
ft shell            # interactive shell, repo at /src/frametop
ft ft-screens ...   # run any program from /opt/frametop
ft test             # the Python suites inside the image
```

`FT_IMAGE` overrides the image reference (default `frametop:local`).

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
it to public in the package settings after the first successful run.

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
