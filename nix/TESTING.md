# Trying the Nix packages on a Frame

A checklist for the first test of the flake and the Home Manager module on a real Steam Frame. [README.md](README.md) explains the design. CI (`.github/workflows/nix.yml`) has already shown that everything builds and starts on aarch64-linux. What it can't show needs the headset: SteamVR, the GPU, Plasma, and the input devices.

**For a coding agent doing this on the headset:** read [AGENTS.md](../AGENTS.md) first. The headset may be on someone's head. Ask before restarting SteamVR or the desktop, and before running any `sudo`. Steps 5 and 6 need both. Everything else here only reads, builds, or writes in the home folder. Write down what each step printed, so that step 9 can report it.

## 0. What's being tested

- **Branch:** `nix-support` of `JRMurr/frametop`.
- **What it adds:**
  - packages for ft-screens, ft-pointer, ft-powerd, the ft_pointer SteamVR driver, and a store tree of the scripts and settings apps (`frametop-apps`);
  - `homeManagerModules.default` (`programs.frametop`), which installs the input relay, pointer, and power services, registers the driver, and installs the launcher override and menu entries;
  - three env hooks in the session script (`FRAMETOP_SCREENS_BIN`, `FRAMETOP_PYTHON`, `FRAMETOP_STARTPLASMA`). With them unset, the scripts behave exactly as before.
- **Main risks, in order:**
  1. Panels may not render, because nixpkgs' Mesa allocates the dmabufs ft-screens hands to SteamVR.
  2. `vrclient.so` may need a library the Nix programs don't link (libGL, libEGL, and libuuid are linked for it).
  3. vrserver may reject the zig-built driver.

## 1. Read-only checks

```sh
nix --version; home-manager --version
ldd --version | head -1                      # expect glibc 2.39
nix shell nixpkgs#binutils -c readelf -d /opt/steamvr/bin/linuxarm64/vrclient.so | grep NEEDED
```

Every NEEDED entry should be glibc's (libc, libm, libdl, libpthread, librt, ld-linux), libstdc++, libgcc_s, or one the Nix programs link for it: libGL, libEGL, libuuid ([packages/vrclient-deps.nix](packages/vrclient-deps.nix)). On SteamOS 0.3.0 the list is exactly those. Anything else means the Nix-built programs can't load SteamVR's client library: stop and report it, or add it to `vrclient-deps.nix`. Step 4's `update-check.py` checks this against the installed programs.

What's installed now (from `install.sh`, or nothing):

```sh
systemctl --user list-unit-files 'frametop*'
ls -la ~/.config/systemd/user | grep frametop
ls -la ~/.local/share/frametop ~/.local/share/applications 2>/dev/null | grep -E 'ft[-_]|deckard|frametop'
cat ~/.config/frametop.conf 2>/dev/null | head -5
```

## 2. Build on the headset, without installing anything

In a checkout of the branch:

```sh
git fetch origin nix-support && git checkout nix-support
nix build -L .#frametop-apps .#ft-pointer-driver
```

There's no binary cache for Frametop's own packages, so ft-screens and friends compile here. That takes a few minutes; nixpkgs' parts download. The driver's build ends with its own check (`newest glibc symbol`, `NEEDED`, `exports: HmdDriverFactory`), and the build fails if that check does.

## 3. Install with Home Manager

**Your Home Manager flake:** add Frametop as an input and enable the module. [README.md](README.md#using-it-from-a-standalone-home-manager-config) has the full example. In short:

```nix
inputs.frametop.url = "github:JRMurr/frametop?ref=nix-support";
# in modules:
frametop.homeManagerModules.default
{ targets.genericLinux.enable = true; programs.frametop.enable = true; }
```

To test a local checkout instead, use `inputs.frametop.url = "git+file:///home/steamos/frametop"` (or pass `--override-input frametop git+file:///home/steamos/frametop` to `home-manager switch`).

**If `install.sh` was used before:** remove what it installed first. Home Manager refuses to replace those files.

```sh
pointer/helper/run.sh uninstall
power/run.sh uninstall
pointer/driver/install.sh uninstall
display-settings/install.sh uninstall
input-settings/install.sh uninstall
desktops.sh uninstall
rm -f ~/.config/systemd/user/frametop-input-relay.service && systemctl --user daemon-reload
```

`frametop-pointer` and `frametop-power` stop when they're uninstalled; that's harmless. Stopping the input relay isn't needed: Home Manager's unit of the same name takes over its fd store.

Then run `home-manager switch` and note everything it prints. In particular, look for:
- the GPU setup warning, with the exact `sudo .../non-nixos-gpu-setup` command;
- `frametop: registered the ft_pointer driver`;
- any "existing file is in the way" error.

## 4. Check what Home Manager installed

```sh
systemctl --user list-unit-files 'frametop*'          # input-relay, pointer, power enabled
readlink ~/.local/share/frametop/ft_pointer           # a /nix/store path
grep ft_pointer ~/.config/openvr/openvrpaths.vrpath   # registered once
grep ^Exec= ~/.local/share/applications/deckard-nested-desktop.desktop   # .../share/frametop/session/frametop-session.sh
python3 scripts/update-check.py                        # see the "vrclient.so" and "pointer driver" lines
```

## 5. GPU setup (needs sudo, so ask)

Run the `sudo .../non-nixos-gpu-setup` command that `home-manager switch` printed. Then:

```sh
ls -l /run/opengl-driver/lib/dri | head      # freedreno/msm drivers should be listed
ls /run/opengl-driver/share/vulkan/icd.d     # freedreno (Turnip) ICD
```

A quick start of ft-screens without SteamVR tests EGL and GBM through `/run/opengl-driver`. It doesn't touch the running desktop:

```sh
mkdir -p /tmp/fttest && chmod 700 /tmp/fttest
XDG_RUNTIME_DIR=/tmp/fttest timeout 5 "$(nix build --no-link --print-out-paths .#ft-screens)/bin/ft-screens" \
  --no-vr --socket ft-test-0 --screen 1280x720@1.0; echo "exit $?"
```

It should run until `timeout` stops it (exit 124), with no EGL, GBM, or renderer errors.

## 6. Restart SteamVR (ask first: it closes everything open in VR)

```sh
systemctl --user restart steamvr.service
```

Then check:

```sh
systemctl --user status frametop-input-relay frametop-pointer frametop-power --no-pager | grep -E '●|Active:'
journalctl --user -u frametop-pointer -n 20 --no-pager
journalctl --user -u frametop-power -n 20 --no-pager
grep -i ft_pointer ~/.local/share/Steam/logs/vrserver.txt | tail -20
./desktops.sh relay status      # from the checkout; vrserver should hold the relay's event devices
python3 scripts/update-check.py # now with SteamVR running: the OpenVR interface checks run too
```

In vrserver.txt, the driver should be found and activated, with no "failed to load" or "interface version" errors.

## 7. The desktop

Launch a program → Desktop.

- **Does it start?** Check `/tmp/frametop-session.log`, `/tmp/frametop-screens.log`, and `pgrep -ax ft-screens`. The ft-screens path should be in `/nix/store`, not in `distrobox`.
- **Do the panels show the desktop?** Black or corrupt panels mean the dmabuf / Mesa risk ([README.md](README.md#known-risks)). If so, report it with `/tmp/frametop-screens.log`.
- **Inside the desktop, check:**
  - the 3D mouse;
  - floating windows (`/tmp/frametop-floatd.log`);
  - the title bar decoration;
  - Frametop Display Settings and Frametop Input Settings: do they open, look like Plasma, and save?
  - Reset Screen Layout (Meta+Shift+R);
  - a desktop restart from Display Settings.
- **Second start:** close the desktop and start it again. It should come up, and the decoration copy shouldn't block it.

## 8. A second switch while SteamVR runs

Run `home-manager switch` again, with no changes. The relay shouldn't restart in a way that loses the mouse, the driver registration shouldn't change, and nothing should break.

## 9. Report

Send back:
- the output of each step, or just the failing one with its log;
- `nix --version`, the SteamOS version (`cat /etc/os-release | grep VERSION`), and the `vrclient.so` NEEDED list.

## Undo

To go back to the `install.sh` setup:
1. Remove the module from the Home Manager config and `home-manager switch` (or `home-manager generations` and activate an older one).
2. Remove the driver registration: `LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrpathreg removedriver ~/.local/share/frametop/ft_pointer`.
3. Run `./install.sh` from the checkout.
