# Frametop packaging: the OCI image as the product

This is the design rationale for packaging Frametop as a CI-built OCI image.
[pack/README.md](README.md) is the practical reference (what the image
contains, the mount matrix, CI); this file explains why it is built this way,
what it solves, and how it is meant to be used.

## The problem today

Frametop on the Frame is three worlds that today get assembled *on the user's
headset* at install time:

- **The C world**: six native binaries (`ft-screens`, `ft-pointer`, `ft-powerd`,
  `ft-gaze`, and friends) built against Qt, OpenVR, and wlroots.
- **The Python world**: the session, gaze, and settings code, with a NumPy /
  OpenCV / PySide6 stack.
- **The host payload**: the SteamVR driver, ft-camd, systemd units, desktop
  files, the KWin script — the parts that must live on the SteamOS host.

The path there is `get.sh` → `install.sh` → `setup/dev-container.sh`: a
distrobox environment is set up, packages are installed via dnf, binaries are
compiled, Python dependencies are fetched. That design has structural costs:

1. **Every install is a build.** The first install downloads 1–2 GB and
   compiles on the device. A flaky mirror, a renamed dnf package, or a failing
   build step fails installation — individually, for every user, at the worst
   possible moment (first contact with the project).
2. **No two installations are alike.** Unpinned dnf and pip installs mean user
   A has one OpenCV and user B another. Bug reports become "works on my
   headset" stories that cannot be reproduced.
3. **SteamOS updates hit the host boundary.** The container's toolchain lives
   in the home folder and survives an update — but every update replaces
   SteamVR, KWin, and gamescope, and anything in the container that points at
   a host path, interface, or quirk can break. `scripts/update-check.py`
   tracks exactly these dependencies. Packaging does not remove this risk —
   the host boundary stays the host boundary — but it shrinks it to what it
   genuinely is: with the toolchain frozen in the image there is no on-device
   build left to rot, and revalidating against a new SteamOS means one CI run
   instead of every headset finding out individually.
4. **Using and developing are coupled.** A user needs a build ecosystem on
   their headset to *run* Frametop. Conversely, a developer tests in an
   environment no user shares.

## The design

> **The OCI image is the product. CI builds it, users pull it, `ft` is the
> only interface in between.**

```
CI (GitHub Actions, native arm64)         Frame / PC
┌──────────────────────────────┐         ┌─────────────────────────────┐
│ pack/Containerfile           │  push → │ ghcr.io/<org>/frametop      │
│  · toolbox base, digest-pinned│        │        ↓ podman pull        │
│  · uv sync --frozen (lockfile)│        │ ~/.local/bin/ft             │
│  · OpenVR tag, stb pinned     │        │  · runs programs            │
│  · six binaries compiled      │        │  · owns the mounts          │
│  · smoke-tested in CI         │        │  · ft update = pull         │
└──────────────────────────────┘         └─────────────────────────────┘
```

The `ft` wrapper holds the image-reference resolution (pinning, updates,
cleanup) and the development commands. How the programs themselves run from
the image on the Frame is still open: see "The runtime on the Frame" below.

## What it solves, point by point

| Today | With the image |
| --- | --- |
| Install = build on the device | `podman pull`. Minutes, no compilers, no dnf |
| Every environment drifts | Every user runs the *exact* CI environment |
| SteamOS updates rot the on-device build | No on-device build left; what an update can still break is the host boundary — revalidated once in CI instead of per headset |
| Users need a developer ecosystem | Users need podman (SteamOS ships it) and the host payload |
| "Works on my headset" | A bug report names an image tag; the developer reproduces it in `ft dev shell` within minutes |
| Shipping fixes | `ft update`. The user never compiles anything |

And the subtler win: **decoupled failure domains.** When Frametop breaks on a
device, it is now either (a) the image — in which case it breaks identically
for everyone and reproducibly in CI — or (b) one of the small, documented host
boundaries. Today (a) and (b) are one soup.

## Why OCI — and not uv alone, not Flatpak

**Why not "just uv"?** uv (lockfile + frozen sync) solves the Python world
properly, and we use it exactly that way *inside* the image. But Frametop is
more than Python: the six C binaries, Qt, OpenVR, wlroots, the compile step
itself. uv freezes no compiler, no dnf package, no `libopenvr_api`. uv alone
still leaves the user compiling on the device with a drifting C toolchain —
the largest source of divergence untouched.

**Why not Flatpak?** Flatpak would be the more "native" answer for desktop
apps on SteamOS, and for the settings GUIs it is not absurd in the long run.
But Frametop is not a collection of desktop apps. It is a set of daemons that
live in SteamVR, on Wayland sockets, on `/dev/shm`, and on localhost sockets —
precisely what Flatpak sandboxing turns into an adventure of `--talk-*` and
`--filesystem` holes. One would spend the effort knocking holes in the sandbox
until it is no longer a sandbox, while still maintaining a separate build
system (flatpak-builder, manifests, a runtime dependency) that does the same
environment-freezing work again, in Flatpak currency. podman, by contrast,
ships with SteamOS (distrobox, which `install.sh` adds, runs on it), and OCI is
the one artifact format that CI, GHCR, and local development all speak
natively.

**Why not "distrobox, but pinned"?** That is what we have today — the dev
container. But a distrobox container is *state on the device* (dnf
transactions accumulating over months, drift), not an *artifact*. The
transition is exactly the point: a container you maintain becomes an image you
replace. `podman pull` is idempotent; a container filesystem with six months
of history is not. (A distrobox *created from* the pinned image, and created
again on every update, is a different thing: it holds no state. It is one of
the runtime options below.)

## How it is used

**Developer (PC, from a checkout):**

```
./ft dev build     # build the image from the checkout
./ft dev test      # the test suites inside the image (strict)
./ft dev shell     # interactive shell, repo at /src/frametop
./ft ft-screens    # run a program
```

Repo mode defaults to the locally built `frametop:local` and mounts the
checkout at `/src/frametop`.

**User (Frame, once the install path exists):**

```
ft update          # pull the published image, pin its digest
```

No repo on the device, no build. The image reference resolves `FT_IMAGE` →
`~/.config/frametop/image` (written by the installer, so installs pin what was
installed) → the published image. Development commands live behind `ft dev`
and are refused in installed mode — a user should not reach the build world by
accident, and an installed wrapper has no checkout to build from anyway.

**Container naming.** Containers run through the wrapper are named
`frametop-<program>-<pid>`: `podman ps` names the program, two runs of one
program don't replace each other, and `ft clean` finds the leftovers of
crashed runs by the prefix.

## What the tests do

Two levels, both running *inside the built image*:

- **`just test` (strict)** — the Python suites, the header-only C tests, and
  a syntax check of every shell script, inside the image. Any failing suite
  fails the run, and `python3` must import the dnf Qt stack and the locked
  packages together, so Qt tests can't pass by skipping. This is the real
  gain over "CI runs pytest on the runner": the tests run in exactly the
  environment the user receives. What is green is green *in the product*.
- **The CI smoke job** — pulls the built image and checks it from the outside:
  the binaries exist, the venv is intact, programs execute and answer. This
  catches broken layers, missing files, and architecture mistakes.

`just lint` (report-only while the pre-existing ruff findings are worked down)
is the on-ramp to strict linting later.

## The runtime on the Frame

Open. Today the programs that run in a container run in the `dev` distrobox,
which is privileged, shares the host's PID, network, and IPC namespaces,
mounts `/dev`, `/sys`, `/tmp`, `/run/user/<uid>`, and the home folder, and
keeps the user's groups (`run.oci.keep_original_groups`). The programs rely on
that:

| Program | Needs from the host |
| --- | --- |
| ft-screens | `/dev/dri/renderD128` (GBM), `XDG_RUNTIME_DIR` (its Wayland socket, for KWin on the host) |
| ft-powerd | `/dev/input` (use), the backlight in `/sys`, writable through the `video` group, `~/.config/frametop.conf`, `~/.cache/frametop` (the brightness to put back after a crash) |
| ft-pointer | `~/.config/frametop.conf`, `/opt/steamvr` (it runs `vrcmd`) |
| ft-gaze, ft-gazepanel | `/dev/shm` (SteamVR's `eye-server.mmap`), `/dev/dri` |
| Settings apps | the Wayland socket and session bus, `distrobox-host-exec` (they run `systemctl --user` on the host) |
| All of them | the host network namespace: they talk over abstract sockets (`@ft_screens`, `@ft_pointer`, ...), and the host's datagram queue length (`net.unix.max_dgram_qlen`, 512 from systemd; a container's own namespace starts at 10, and the input relay's burst of releases then loses its last ones, which leaves buttons held) |

OpenVR clients need more, found on the device with a containerized ft-powerd
(SteamOS 0.4.3, SteamVR 2.18.2): the path registry `~/.config/openvr`, also at
the absolute `/home/steamos/...` paths it names; SteamVR's IPC control file in
`/tmp` (with a private `/tmp`, `VR_Init` fails with `Init_Internal` 124);
`HOME` set explicitly; and no `--user`, since rootless podman maps the
container's root to the desktop user and a forced uid breaks that mapping.

A `podman run` with a hand-picked list of mounts (the wrapper's first
`FT_FRAME=1` mode) got ft-powerd connected to SteamVR, but it had no
`/dev/dri`, `/dev/input`, writable `/sys`, host groups, config files, or
`XDG_RUNTIME_DIR`, so the programs couldn't do their jobs. Two ways give them
what the dev container gives them:

1. **A distrobox created from the pinned image.** The image keeps `sleep
   infinity` as its command for this. It gets every mount, group, and
   namespace above with no list to maintain; the units keep `distrobox
   enter` and point at `/opt/frametop/bin`. An update creates the box again
   from the new digest, so it holds no state. Its first start runs
   distrobox's own setup, which once made installs over SSH stop at a sudo
   prompt (issue #9).
2. **Quadlet units** (podman 5.5 on SteamOS ships the generator) with the
   same flags as distrobox: privileged, host PID, network, and IPC, the same
   mounts, the user's groups. No distrobox setup step, and systemd tracks the
   container itself rather than a `podman` client.

Either way the image adds no isolation (the dev container has none either).
What it adds is a pinned environment that was built and tested before it
reached the headset. Starting a program costs about the same: on the Frame a
`podman run` starts in about 0.23 s, `distrobox enter` in about 0.35 s.
Rootless storage lands in `~/.local/share/containers`, shared with Valve's
`lepton-*` containers (see README.md).

## What `install.sh` does in this world

1. `podman pull` the image by digest, write the reference to
   `~/.config/frametop/image`
2. copy the wrapper to `~/.local/bin/ft` (already installed-mode capable)
3. set up the runtime (a distrobox from the image, or Quadlet units) and
   install the units, pointing at `/opt/frametop`
4. the host payload from the same release: SteamVR driver registration
   (`vrpathreg`), the KWin script, desktop files, and the optional parts that
   need sudo (ft-camd's capabilities, the eye tracker's frame grabber, the
   Bluetooth fixes)

`get.sh` stays the front door, and the FrameDrop package runs the same steps;
the difference is that step 1 ships the frozen image instead of building on
the device. The image and the host payload come from one tagged commit.

## Open decisions

1. **Tagging**: decided — `:latest` is refused by the wrapper (see
   pack/README.md, "Never :latest"). Releases cut version tags; `ft update`
   pins the digest of whatever version tag `install.sh` recorded. What
   remains open is only the cadence: a tag per release vs. per CI build.
2. **Registry home**: `ghcr.io/deejanuz/frametop`, published from `main` and
   `v*` tags only.
3. **Pull without auth**: depends on package visibility; install.sh can pin
   the reference either way.
4. **Settings apps**: currently dnf-provided (PySide6/Kirigami) and run via
   the image. Whether the GUIs migrate toward Flatpak/host packages later is left
   open deliberately.
5. **Host payload distribution**: payload tarball + `get.sh` as artifact
   installer is the current proposal.
6. **Runtime on the Frame**: a distrobox from the image, or Quadlet units
   (see above). Decided by a headset trial, which also measures the install
   time against today's on-device build.
