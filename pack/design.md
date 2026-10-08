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

The `ft` wrapper is deliberately the single point where "how does Frametop run
in a container" lives: image-reference resolution, mounts, IPC, network,
container names. The systemd units become one-liners
(`ExecStart=%h/.local/bin/ft ft-powerd`). There is exactly one file to read or
change when the container integration moves.

## What it solves, point by point

| Today | With the image |
| --- | --- |
| Install = build on the device | `podman pull`. Minutes, no compilers, no dnf |
| Every environment drifts | Every user runs the *exact* CI environment |
| SteamOS updates rot the on-device build | No on-device build left; what an update can still break is the host boundary — revalidated once in CI instead of per headset |
| Users need a developer ecosystem | Users need podman (SteamOS ships it) and nothing else |
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
environment-freezing work again, in Flatpak currency. OCI/podman, by contrast,
is already the substrate SteamOS ships (distrobox is built on it), and OCI is
the one artifact format that CI, GHCR, and local development all speak
natively.

**Why not "distrobox, but pinned"?** That is what we have today — the dev
container. But a distrobox container is *state on the device* (dnf
transactions accumulating over months, drift), not an *artifact*. The
transition is exactly the point: a container you maintain becomes an image you
replace. `podman pull` is idempotent; a container filesystem with six months
of history is not.

## How it is used

**Developer (PC, from a checkout):**

```
./ft dev build     # build the image from the checkout
./ft dev test      # the test suites inside the image (strict)
./ft dev shell     # interactive shell, repo at /src/frametop
./ft ft-screens    # run a program
```

Repo mode defaults to the locally built `frametop:local` and mounts the
checkout at `/src/frametop`. This is the intended long-term replacement for
the ad-hoc `frametop-dev` container and `tools/local-dev`.

**User (Frame, after the install slice):**

```
ft ft-powerd       # the units do this; manually identical for debugging
ft update          # pull the published image
```

No repo on the device, no build. The image reference resolves `FT_IMAGE` →
`~/.config/frametop/image` (written by `install.sh`, so installs pin what was
installed) → the published image. Development commands live behind `ft dev`
and are refused in installed mode — a user should not reach the build world by
accident, and an installed wrapper has no checkout to build from anyway.

**Container naming.** Containers run through the wrapper get stable names,
`frametop-<program>` (`frametop-ft-powerd`), so `podman ps`, `podman logs`,
and the unit names tell one story. A leftover with the same name (a crashed
run) is replaced on start. Consequence: one instance per program — matching
the system's shape, since the units are not templated.

## What the tests do

Two levels, both running *inside the built image*:

- **`just test` (strict)** — the Python suites (pytest) inside the image. This
  is the real gain over "CI runs pytest on the runner": the tests run in
  exactly the environment the user receives. What is green is green *in the
  product*.
- **The CI smoke job** — pulls the built image and checks it from the outside:
  the binaries exist, the venv is intact, programs execute and answer. This
  catches broken layers, missing files, and architecture mistakes.

`just lint` (report-only while the pre-existing ruff findings are worked down)
is the on-ramp to strict linting later.

## The runtime on the Frame (designed, to be validated on the device)

```
ghcr.io/<org>/frametop:<tag>       the image
~/.local/bin/ft                    the wrapper (SteamOS root is read-only: no /usr/local)
~/.config/frametop/image           the pinned reference from install.sh
~/.config/systemd/user/*.service   the units, ExecStart=%h/.local/bin/ft …
```

podman assumptions to verify in a device session: rootless storage should land
in `~/.local/share/containers` (since `/var/lib/containers` belongs to the
read-only root), and unauthenticated pulls need the ghcr package to be public
(or the install flow pins the reference from an authenticated pull).

The Frame's container additions are gated behind `FT_FRAME=1` until validated
there — see the mount matrix in [pack/README.md](README.md). First device
tests, in order: `ft ft-powerd` (talks to OpenVR only), then `ft ft-screens`
in a session, then the units.

## What `install.sh` does in this world

1. `podman pull` the image (or load a payload tarball), write the reference to
   `~/.config/frametop/image`
2. copy the wrapper to `~/.local/bin/ft` (already installed-mode capable)
3. install the units — unchanged content, `ExecStart=` now the wrapper
4. the host payload as today: SteamVR driver registration (`vrpathreg`),
   ft-camd setcap, the KWin script, desktop files

`get.sh` stays the front door; the difference is that step 1 ships the frozen
image instead of building on the device. The host payload tarball (with
checksums, attached to releases) is the following slice.

## Open decisions

1. **Tagging**: decided — `:latest` is refused by the wrapper (see
   pack/README.md, "Never :latest"). Releases cut version tags; `ft update`
   pins the digest of whatever version tag `install.sh` recorded. What
   remains open is only the cadence: a tag per release vs. per CI build.
2. **Registry home**: the fork's ghcr for now; the question of moving the
   package under the upstream org belongs in the upstream discussion.
3. **Pull without auth**: depends on package visibility; install.sh can pin
   the reference either way.
4. **Settings apps**: currently dnf-provided (PySide6/Kirigami) and run via
   `ft`. Whether the GUIs migrate toward Flatpak/host packages later is left
   open deliberately.
5. **Host payload distribution**: payload tarball + `get.sh` as artifact
   installer is the current proposal.
