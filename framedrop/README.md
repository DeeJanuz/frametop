# Install with FrameDrop (proof of concept)

[FrameDrop](https://framedropvr.com) is a Windows app that sideloads onto a Steam Frame: it copies a build to the headset and adds it to the Steam library. Issue #25 asks for an "Install with FrameDrop" button. Frametop isn't an app FrameDrop can copy over as is: it installs user services, a SteamVR driver, and a container, and two optional parts need sudo. So FrameDrop installs a small installer instead. Playing "Frametop" from the library opens a window that asks what to install, and your password for the parts that need it, then runs the one-line installer (`get.sh --yes`) and shows its progress. A download built with a release list installs that release, built, from its image (`get.sh --release`, see [pack/README.md](../pack/README.md)): nothing compiles on the headset.

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
2. It clears Steam's preload and library paths, and opens `installer/progress.py` (GTK 4 and libadwaita).
3. The window asks which optional parts to install: our own eye tracker (on by default) and the Bluetooth fixes. Both need sudo, so it asks for your SteamOS password and checks it with `sudo -v`. If your user has no password (SteamOS starts without one), it says how to set one and leaves both out.
4. It runs the zip's own `get.sh --yes` in a transient user service, `frametop-framedrop-install`, with `--release --manifest frametop-releases.json` when the zip has a release list. The service is used because Steam ends the title's whole process tree when it's quit, and starts it with an OOM score of 900.
5. It follows the service's log, shows the steps as a progress bar, and reports the result. Closing it leaves the install running. Playing the title again reattaches.

`--yes` keeps the version that's installed, or installs stable, and skips the SteamVR restart. The window says to restart SteamVR.

### The password

FrameDrop has no way to pass a password along, and the zip is the same file for everyone, so the password is typed on the headset, in the window. It stays in the window's memory until the install ends:

- The service gets `SUDO_ASKPASS=installer/askpass`. When install.sh's sudo asks, askpass connects to a socket the window keeps in `/run/user/UID/frametop-install` (mode 0700), and the window answers only a process in the install's own service (checked by its peer credentials and cgroup). If it doesn't have the password yet (the window was opened again), it asks you for it, or you skip that part.
- The password is never written to a file, a log, the service's environment, or a command line. The window wipes its copy when the install ends or the window closes. Strings Python and GTK made from it along the way can't be wiped; they go with the process.
- With the window closed there's nobody to answer: sudo fails, install.sh says which parts it skipped, and playing Frametop again finishes them.

`--dry-run` gets the files into `~/.cache/frametop-framedrop/dry-run` and stops there (`get.sh --clone-only`).

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
framedrop/build.sh [--releases FILE] [ZIP_URL]
```

This writes `framedrop/build/Frametop.zip` (reproducible: the installer, this checkout's `get.sh`, and with `--releases`, a release list from `pack/release-manifest.py`) and `frametop.framedrop.json`, FrameDrop's manifest with the zip's sha256. By default, `ZIP_URL` points at a `framedrop-installer` release asset. The button's link would be `https://framedropvr.com/install?manifest=https://deejanuz.github.io/frametop/frametop.framedrop.json`, with the manifest committed to main for Pages.

## Open questions, for a test with FrameDrop on a Windows PC

- Does FrameDrop keep or set the exec bit on `frametop-install.sh`? A zip unpacked on Windows loses it, and without it nothing runs.
- What start command does FrameDrop pick for this zip, and which runtime?
- Does the manifest's `name` become the Devkit Game name? It has to stay free of `-`.
- Can you type the password in the window in VR: does the SteamVR keyboard come up for its password field?
