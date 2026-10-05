# Working on Frametop

Rules for people and coding agents changing this repo. The README covers what Frametop is and how to install it; `docs/reference.md` covers each component, and `docs/design.md` records how SteamVR on the Frame behaves and why things are built the way they are. Read its notes on SteamVR before changing how the screens, the pointer, or the input relay talk to SteamVR.

## Two ways to run the scripts

Every script works in both modes, and must keep working in both:

- On the Frame (SteamOS, VR variant), commands run locally, in this checkout.
- From a PC over SSH, the repo is synced to `~/dev/frametop` on the Frame (`scripts/sync.sh`), and commands run there. `scripts/_env.sh` works out which mode applies (`FRAME_LOCAL`, `FRAME_HOST`, `FRAME_REPO`).

```
scripts/sync.sh                          # PC -> ~/dev/frametop on the Frame
scripts/frame.sh -C <dir> '<build cmd>'  # runs in the "dev" Fedora distrobox
scripts/frame.sh --host '<cmd>'          # runs on the SteamOS host
```

- From a PC, edit only on the PC. The Frame's copy is a mirror that `sync.sh` overwrites.
- Build inside the `dev` container. The SteamOS host has a read-only root and no compilers. Container builds link against the container's libraries, so they run in the container (`distrobox enter dev -- ...`); the SteamVR driver is built to run on the host.
- Container packages the build needs go in the list in `setup/dev-container.sh`, so the container can be rebuilt.
- Build output goes in `build/` next to the sources. It's gitignored and never synced.

## The headset may be in use

A Steam Frame is someone's personal headset, and they may be wearing it while you work.

- Don't kill or restart `gamescope`, `steam`, `vrserver`, `vrcompositor`, the gamescope session, or the Frametop desktop without asking. Each one ends or disrupts whatever is happening in VR.
- Don't run host `sudo`, `steamos-readonly disable`, `steamos-devmode` changes, pacman installs, or reboots without explicit approval. Three installers need host `sudo`, and they ask for it: the Bluetooth fixes (`setup/bluetooth/install.sh`), hand tracking (`hands/run.sh install` and `caps`, which set ft-camd's file capabilities with `setcap`), and our own eye tracker's frame grabber (`gaze/tracker/install.sh`).
- Write only inside the repo, `/tmp`, and the container unless told otherwise. The installers are the exception: they write the user services, launchers, and the SteamVR driver into the home folder. The Bluetooth fixes and the eye tracker's frame grabber also install root-owned files and system services under `/etc` (`/etc/steamframe`, `/etc/frametop`, `/etc/systemd/system`). When an installer starts writing something new outside the repo, add it to `uninstall.sh` too: users uninstall with that script, not with each installer's `uninstall`.
- Never copy `.netrc`, SSH keys, or Steam config off the Frame or into this repo.

## SteamOS updates

A SteamOS update replaces SteamVR, KWin, and gamescope with the rest of the OS image. When a change starts depending on something from the image (a host file, an OpenVR interface outside the bundled header, an undocumented layout or output format, a SteamVR or KWin quirk), add a check for it to `scripts/update-check.py`, or a retest hint for its package there. [docs/design.md](docs/design.md) has the background.

## The image (`pack/`)

`pack/Containerfile` builds the OCI image in which Frametop runs; [pack/design.md](pack/design.md) explains why and [pack/README.md](pack/README.md) documents the specifics. Rules:

- **Pin every input.** Base images by digest, Python via `uv.lock` (commit the lock, never a bare `uv pip install`), vendored headers and libraries by tag, CI actions by commit SHA. Anything that floats makes users' environments drift again, which is the problem the image exists to solve.
- **`ft` is the single integration point.** Container names, mounts, the image-reference chain, and the dev/runtime mode split live in the wrapper, not in the units or install scripts. Keep it working in both homes (a repo checkout, and installed in `~/.local/bin` without a repo) and in both modes on the Frame (`FT_FRAME=1`), and under both docker and podman. New container integration goes there, gated behind `FT_FRAME=1` until validated on the device.
- **Build and test through the image.** `./ft dev build` / `./ft dev test` are the reference path; `just test` must stay green inside the image, and CI (`pack/`-triggered workflow) is the gate. Keep the containerfile buildable on arm64 — that is the only architecture the Frame has.
- **Host dependencies stay explicit.** When a change starts depending on a SteamOS host file, a SteamVR quirk, or a mount that only exists on the device, say so in `pack/README.md` and add an update-check if it can break on a SteamOS update.

## Names

User-facing names are "Frametop", "Frametop Display Settings", and "Frametop Input Settings". Programs and files use the `ft-` / `ft_` prefix (`ft-screens`, `ft-pointer`, `ft-layout`, the `ft_pointer` driver); config, units, and overlay keys use `frametop`. Program names must stay within 15 characters: Linux truncates process names there, and the scripts find programs with `pgrep -x` / `pkill -x`.
