# Frametop with Nix and Home Manager

The flake builds Frametop with nixpkgs instead of the Fedora `dev` container. The container exists only because SteamOS's libraries are too old to build against, and nixpkgs replaces it. A Home Manager module then sets up what `install.sh` sets up. This is for a Steam Frame (SteamOS, aarch64-linux, not NixOS) with Nix and standalone Home Manager.

It covers the multi-screen desktop, the input relay, the 3D mouse, the power service, and the two settings apps. It doesn't cover gaze mode, hand tracking, remote desktop (`REMOTE=1` still enters the container), or the Bluetooth fixes. Use the repo's installers for those.

## Packages

| Package | What it is |
| --- | --- |
| `ft-screens` | The compositor (`screens/build.sh`), and `ft-handtest` |
| `ft-pointer` | The 3D mouse's helper (`pointer/helper/build.sh`) |
| `ft-powerd` | The power service (`power/build.sh`) |
| `ft-pointer-driver` | The `ft_pointer` SteamVR driver, laid out as SteamVR expects (`share/frametop/ft_pointer`) |
| `frametop-apps` (`default`) | The scripts, Python tools, and both settings apps as one tree in `share/frametop`, with the three programs above inside it |
| `frametop-scripts` | The same tree without the settings apps, so no Qt |

`share/frametop` mirrors the repo. The scripts find each other by relative path (`$here/../layout/ft-layout`), and ft-screens finds `ft-layout` from its own path, so each program sits where the repo's build would put it (`screens/build/ft-screens`). The settings apps live in the same tree because they import `ft_layout` and call `desktops.sh` by relative path.

The repo's scripts have three env hooks: `FRAMETOP_SCREENS_BIN` (run ft-screens on the host, not in the container), `FRAMETOP_PYTHON`, and `FRAMETOP_STARTPLASMA`. The package sets their defaults to store paths. It exports nothing, so the host Plasma that the session starts gets a clean environment, with no `LD_LIBRARY_PATH` and no Qt paths.

ft-screens (EGL, GLES, GBM) has `/run/opengl-driver/lib` first in its RUNPATH. The settings apps reach the same drivers through nixpkgs' libglvnd, which looks there too.

The driver is the exception, because it loads into the host's `vrserver`. `zig c++` builds it against glibc 2.39's symbol versions with libc++ linked in. It exports only `HmdDriverFactory`, needs only libc and libm, and has no RUNPATH. The package's install check enforces all of this, like `pointer/driver/build.sh`.

## Using it from a standalone Home Manager config

```nix
# flake.nix of your Home Manager config
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
    home-manager = {
      url = "github:nix-community/home-manager";
      inputs.nixpkgs.follows = "nixpkgs";
    };
    frametop = {
      url = "github:JRMurr/frametop";
      # One nixpkgs for Frametop and the GPU drivers (see Known risks). Your nixpkgs needs
      # wlroots_0_20; drop this line to use the nixpkgs pinned in Frametop's flake.lock.
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = { nixpkgs, home-manager, frametop, ... }: {
    homeConfigurations.steamos = home-manager.lib.homeManagerConfiguration {
      pkgs = nixpkgs.legacyPackages.aarch64-linux;
      modules = [
        frametop.homeManagerModules.default
        {
          home.username = "steamos";
          home.homeDirectory = "/home/steamos";
          home.stateVersion = "25.11";

          targets.genericLinux.enable = true;  # also turns on targets.genericLinux.gpu
          programs.frametop.enable = true;
          # programs.frametop.pointer.enable = false;  # no 3D mouse
          # programs.frametop.power.enable = false;    # no display power service
          # programs.frametop.launcher.enable = false; # the launcher keeps the stock desktop
        }
      ];
    };
  };
}
```

`programs.frametop` sets up:

- **`frametop-input-relay`**: the same unit as `input/frametop-input-relay.service` (`Type=notify`, `Before=steamvr.service`, the fd store), run with the package's Python. Home Manager starts new units that active targets want. Started for the first time under a running SteamVR, the relay would take the mouse away from it, so an `ExecCondition` skips that start. The relay comes up before SteamVR the next time SteamVR starts. Restarts still go ahead, because the fd store keeps the same devices.
- **`frametop-pointer`** and **`frametop-power`**: they start and stop with SteamVR, as upstream. `ExecStart` points straight at the store, with no container.
- **The ft_pointer driver**: it's linked at `~/.local/share/frametop/ft_pointer`, the path `pointer/driver/install.sh` uses. Activation registers it once with the host's `vrpathreg`. The path stays the same across generations, so a rebuild doesn't touch SteamVR's config.
- **Menu entries**: the launcher's Desktop entry (the `deckard-nested-desktop.desktop` override that `desktops.sh install` writes), Frametop Display Settings, Frametop Input Settings, Reset Screen Layout, Hide/Show Screens, and their shortcuts. Activation rewrites each profile's entry (`ft-layout launchers`), since those entries point into the store.
- **`~/.config/frametop.conf`**: it isn't managed, because the settings apps write to it. A missing one is created from the example, with `POINTER=1` if the 3D mouse is on.

The session copies the title bar decoration into `~/.local/share/kwin/decorations` itself each time it starts, as it does upstream, so Home Manager doesn't manage that copy. Home Manager would only fight the session's `rm -rf`/`cp`.

Host programs are options under `programs.frametop.host`: `steamvr` (`/opt/steamvr`), `startPlasma`, `vrpathreg`, and `kwriteconfig`. Don't add any `kdePackages` to `home.packages`. `~/.nix-profile/bin` comes first in PATH, so the session would start those programs instead of the host's.

The repo's scripts still work from the tree, for example `$(dirname $(readlink -f $(which ft-layout)))/../desktops.sh status`. Don't use `desktops.sh install`, `uninstall`, or `relay install`: Home Manager owns those files.

### GPU setup (once)

ft-screens and the settings apps use Mesa from nixpkgs, through `/run/opengl-driver`. Home Manager's `targets.genericLinux.gpu` provides that path (it's on by default with `targets.genericLinux.enable`). After the first `home-manager switch`, activation prints the command to run once:

```sh
sudo /nix/store/...-non-nixos-gpu-setup/bin/non-nixos-gpu-setup
```

It installs a systemd unit that links `/run/opengl-driver` to the current drivers at boot. Run it again when activation says the drivers need an update. It needs host `sudo`. AGENTS.md asks for explicit approval for that on someone's headset.

### Moving from install.sh

Home Manager won't replace files the installers wrote. Remove those first, from the checkout:

```sh
pointer/helper/run.sh uninstall
power/run.sh uninstall
pointer/driver/install.sh uninstall   # Home Manager registers the same path again
display-settings/install.sh uninstall
input-settings/install.sh uninstall
desktops.sh uninstall
rm ~/.config/systemd/user/frametop-input-relay.service && systemctl --user daemon-reload
```

`desktops.sh relay uninstall` also clears the relay's fd store. That works too, but then SteamVR needs a restart to see the new devices. Either way, restart SteamVR once after the first switch, so it loads the driver and the relay comes up before it. Restarting SteamVR closes everything open in VR.

## Building and checking

```sh
nix flake check --all-systems                 # evaluates everything; builds the driver and frametop-scripts
nix build .#frametop-apps                     # on the Frame, or an aarch64 builder
nix build .#packages.x86_64-linux.ft-screens  # the same derivations on a PC
```

`checks` builds `ft-pointer-driver`, so its glibc, NEEDED, RUNPATH, and exports checks run, and it builds `frametop-scripts`. The settings apps' Qt closure stays out of `nix flake check`.

CI (`.github/workflows/nix.yml`) runs on GitHub's arm runner, natively on aarch64-linux like the Frame, with Determinate Nix and the Magic Nix Cache, so a run rebuilds only what changed. It evaluates the flake for both systems, runs the aarch64 checks, builds every package, and builds a Home Manager configuration with `programs.frametop` on (`nix/ci/home.nix`; run it yourself with `nix build --impure -f nix/ci/home.nix`). It also starts both settings apps with no display, to check that their QML loads.

## Known risks

These were checked on x86_64 and with aarch64 cross builds of the driver, ft-pointer, and ft-powerd, not on a Frame.

- **Zero-copy dmabufs with nixpkgs' Mesa.** ft-screens hands its buffers to SteamVR (`ImportDmabuf`) without copying. With nixpkgs' Mesa (freedreno/Turnip) allocating them in place of Valve's SteamOS Mesa, the formats and modifiers SteamVR accepts may differ. If panels come out black or corrupt, try SteamOS's Mesa version: `targets.genericLinux.gpu.packages` takes a package set whose `mesa` is overridden.
- **`vrclient.so` loaded into Nix processes.** nixpkgs' `libopenvr_api` loads `/opt/steamvr/bin/linuxarm64/vrclient.so` into ft-screens, ft-pointer, and ft-powerd. A library loaded that way finds its dependencies only among libraries the process already has, or in nixpkgs' glibc. The programs' RUNPATH doesn't apply to it, and `/usr/lib` is never searched. On the Frame, `vrclient.so` also needs libGL, libEGL, and libuuid, so each program links nixpkgs' copies itself ([packages/vrclient-deps.nix](packages/vrclient-deps.nix)), and each package's install check fails if one is missing. A SteamVR update that adds a library breaks this again: `scripts/update-check.py` compares `vrclient.so`'s libraries with each Nix program's, and names any that are missing. Add those to `vrclient-deps.nix`.
- **One Mesa.** `libgbm` comes from the Frametop flake's nixpkgs, and the GBM/DRI backends in `/run/opengl-driver` come from the nixpkgs of your Home Manager config. Keep them the same (`inputs.nixpkgs.follows`), or point `targets.genericLinux.gpu.packages` at Frametop's.
- **Newer OpenVR headers.** ft-pointer, ft-powerd, and the driver now build against openvr 2.15.6's headers, not the older ones bundled with SteamVR. ft-screens already used 2.15.6, so the runtime serves its client interfaces, and `update-check.py` checks the client ones. Nothing checks the driver's server interfaces, so look for `ft_pointer` in `vrserver.txt` (`pointer/driver/install.sh log`) after the first start.
- **Replacing the driver while SteamVR runs.** A switch that changes the driver moves the link to a new store path under the running vrserver. Upstream notes that a replaced driver's bindings misbehave until SteamVR restarts, so restart SteamVR after a driver change.
- **Qt paths in the settings apps' children.** The apps' Qt wrapper sets `QT_PLUGIN_PATH` and the QML import paths for the app. Host programs the apps run, such as `kwriteconfig6` and `systemctl`, inherit those paths. Qt rejects plugins built for another Qt version, so this should be harmless. Restarting the desktop goes through `systemd-run`, which doesn't inherit them.
- **Turning the 3D mouse off** leaves the driver registered, pointing at a path that no longer exists, and SteamVR skips it. To remove the registration: `LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrpathreg removedriver ~/.local/share/frametop/ft_pointer`.
