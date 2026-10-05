# Install with FrameDrop (proof of concept)

[FrameDrop](https://framedropvr.com) is a Windows app that sideloads onto a Steam Frame: it copies a build to the headset and adds it to the Steam library. Issue #25 asks for an "Install with FrameDrop" button. Frametop isn't an app FrameDrop can copy over as is: it builds in a container, installs user services and a SteamVR driver, and asks questions in a terminal. So FrameDrop installs a small installer instead. Playing "Frametop" from the library runs the one-line installer (`get.sh --yes`) and shows its progress.

Nothing here is published yet: no release asset, no manifest on Pages, no button.

## How FrameDrop installs a Linux zip

It uses Valve's SteamOS Devkit path: pair once with the headset's devkit service, then rsync the unpacked zip into `~/devkit-game/<name>` over SSH, and register it with Steam as a Devkit Game with a start command. `devkit.sh` here makes the same calls with Valve's devkit-utils, so all of this can be tested on the Frame without a PC.

## What the probe found (SteamOS 0.3.0, build 20260922.6101926)

`probe/probe.sh`, started as a Devkit Game, recorded:

- Devkit titles run on the host, not in a container, as user `steamos`, from a process tree that Steam's reaper owns. This held even with the compat tool set to `SteamLinuxRuntime_4-arm64`: Steam recorded the mapping and still ran it on the host.
- On the host, everything the installer needs works: git, curl to GitHub, podman (sees the `dev` container), `systemctl --user`, `systemd-run --user`, and GTK 4 with libadwaita.
- A GTK window opens in gamescope (an X11 window on `:1`, drawn through gamescope's Vulkan WSI).
- Steam puts its overlay in `LD_PRELOAD` and Steam runtime paths in `LD_LIBRARY_PATH` and `PATH`. Every host tool prints a preload error unless they're cleared.
- Inside the Steam Linux Runtime 4 container (started by hand with its `run` script), there's no git, podman, systemctl, or GTK, but `flatpak-spawn --host` runs commands on the host.
- Steam refuses Devkit Game names with a `-` ("missing/invalid arguments").
- Steam doesn't make the start command executable: without the exec bit, the title exits in a second and nothing runs.

## The installer

`installer/frametop-install.sh` is the start command.

1. In the container, it starts itself again on the host with `flatpak-spawn --host`.
2. It clears Steam's preload and library paths.
3. It runs `curl get.sh | bash -s -- --yes` in a transient user service, `frametop-framedrop-install`, unless one is already running. The service is used because Steam ends the title's whole process tree when it's quit, and starts it with an OOM score of 900.
4. `installer/progress.py` (GTK 4 and libadwaita) follows the service's log, shows `install.sh`'s steps as a progress bar, and reports the result. Closing it leaves the install running. Playing the title again reattaches.

`--yes` keeps the version that's installed, or installs stable. It skips the parts that need sudo (our eye tracker and the Bluetooth fixes) and the SteamVR restart. The window says to restart SteamVR.

`--dry-run` clones into `~/.cache/frametop-framedrop/dry-run` and stops there (`get.sh --clone-only`).

## Try it on the Frame

```
framedrop/devkit.sh add FrametopTest framedrop/installer "./frametop-install.sh --dry-run"
framedrop/devkit.sh run FrametopTest        # or Play it from the Steam library
framedrop/devkit.sh remove FrametopTest
framedrop/devkit.sh add FrametopProbe framedrop/probe "./probe.sh native"   # the probe
```

The probe writes `~/.cache/frametop-framedrop/probe-native.log`, and the installer writes `~/.cache/frametop-framedrop/install.log`.

## Build the download

```
framedrop/build.sh [ZIP_URL]
```

This writes `framedrop/build/Frametop.zip` (reproducible) and `frametop.framedrop.json`, FrameDrop's manifest with the zip's sha256. By default, `ZIP_URL` points at a `framedrop-installer` release asset. The button's link would be `https://framedropvr.com/install?manifest=https://deejanuz.github.io/frametop/frametop.framedrop.json`, with the manifest committed to main for Pages.

## Open questions, for a test with FrameDrop on a Windows PC

- Does FrameDrop keep or set the exec bit on `frametop-install.sh`? A zip unpacked on Windows loses it, and without it nothing runs.
- What start command does FrameDrop pick for this zip, and which runtime?
- Does the manifest's `name` become the Devkit Game name? It has to stay free of `-`.
