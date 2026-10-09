# Frametop

Frametop is a desktop for the Steam Frame that runs on the headset itself, with no PC. Put several monitors around you, pull app windows out to float on their own, and point with a mouse, or with your eyes.

- **Screens that are real monitors.** Each KDE Plasma screen has its own resolution and shape: an ultrawide in front, portrait screens beside it. Move, resize, curve, and roll them, or pin one to your wrist or your head. They come back to your layout when the desktop starts.
- **Windows that float on their own.** Take any app window off the screens into a panel of its own with Meta+Shift+F, its title bar button, or Launch as Standalone. It stays part of the desktop, so drag and drop and the clipboard still work between floating windows and the screens.
- **Profiles.** Save where your screens are, which ones show, and which apps are open where. Switch to a profile from Display Settings, a key, or its launcher entry, or start the desktop in one.
- **One mouse for all of SteamVR.** A Bluetooth mouse drives a small dot anchored in the room. It works the screens, the dashboard, Steam, and overlays, and hands the laser back when you pick up a controller.
- **Look and click (experimental).** In gaze mode the pointer goes where you look. Meta+J and Meta+K, or the mouse buttons, click and fine-tune, and each correction teaches the tracker. Calibration runs in the headset.
- **Made for long sessions.** The displays turn off when the headset isn't used, even on a stand that makes it seem worn. It stays awake on the charger, and you can reach the desktop remotely over VNC.

Two settings apps come with it: Frametop Display Settings for the screens, profiles, and power, and Frametop Input Settings for mice, keyboards, gaze, and button mappings. Optional fixes let Bluetooth LE mice and keyboards like the Swiftpoint Z3 reconnect after they sleep. Install it with one command: see [Install on the headset](#install-on-the-headset).

Frametop is an independent project, not made by or affiliated with Valve.

Join the [Frametop Discord](https://discord.gg/W3X9f7z3Bc) for questions, ideas, and help with your setup.

## Install on the headset

> **SteamOS 0.4:** SteamOS 0.4 moved the eye tracker's data that gaze mode reads. This version of Frametop reads both SteamOS 0.3's and 0.4's, and it's tested on 0.4.5. Run `scripts/doctor.sh` after the update: it says whether the eye tracker's layout is one Frametop knows. It also says whether the update deleted the Bluetooth fixes or our eye tracker's frame grabber, which happens when they were installed by Frametop 0.3.0-exp.3 or older. Reinstall what it names (`setup/bluetooth/install.sh install`, `gaze/tracker/install.sh`). From then on they're kept through updates.

You need a Steam Frame with an internet connection, a keyboard (Bluetooth, or the on-screen one), and about 3 GB of free space.

1. In the launcher, choose Launch a program → Desktop.
2. In the application menu, open System → Konsole.
3. Run:

   ```
   curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash
   ```

   It asks which version you want: stable (the `main` branch, tested releases) or experimental (the `experimental` branch, the newest features, less tested). Then it clones the repo into `~/frametop` and runs `install.sh`. To choose without the question, add `-s -- --stable` or `-s -- --experimental` after `bash`. By hand, the same is `git clone https://github.com/DeeJanuz/frametop.git ~/frametop`, then `cd ~/frametop` and `./install.sh` (add `--branch experimental` to the clone for experimental).

   The third choice, experimental release, downloads Frametop already built (`Frametop.zip`, about 1.1 GB, from the [Frametop organization's releases](https://github.com/Frametop/frametop/releases)) and installs it without compiling anything. `-s -- --release --experimental` picks it without the question.

   The installer sets up distrobox in your home folder (the system files aren't touched), a Fedora build container, and everything else. The first run downloads 1–2 GB. It asks you four things along the way: whether to install gaze mode (experimental, yes by default), our own eye tracker for it (yes by default), and the Bluetooth fixes, then whether to restart SteamVR. The eye tracker and the Bluetooth fixes need your `sudo` password; if you've never set one, run `passwd` first, or skip them for now. SteamVR has to restart once at the end, which closes everything open in VR, including the terminal. Rebooting the headset works too.

After the restart, Launch a program → Desktop opens the multi-screen desktop, with its screens arranged around where you're facing. Frametop Display Settings and Frametop Input Settings are in the desktop's application menu, under Settings. SteamOS's own single-screen desktop is still there, as Native Desktop in the same list.

If you work in the desktop for long stretches, or leave the headset on a stand, open Frametop Display Settings → Power. Turn on Stay awake while plugged in: by default Steam puts the Frame to sleep after an hour without input, even while it charges. And choose when the displays turn off while the headset isn't used. SteamVR turns them off a few seconds after you take the headset off, but a stand or mount that covers the proximity sensor inside it makes the headset seem worn, and its displays stay on all night.

### Add a Bluetooth mouse or keyboard

1. Pair it in Steam, under Settings → Bluetooth.
2. If you installed the Bluetooth fixes, apply them once for the new device with Frametop Input Settings → Bluetooth → Apply Bluetooth fixes (or `setup/bluetooth/install.sh run`). After that it reconnects on its own.
3. Move the mouse, and the dot appears where you're looking.

## Use

### Screens

| Do this | To get this |
| --- | --- |
| Point near the bottom of a screen | Its controls fade in: the bar, the curve and roll buttons, and the resize tab on the corner |
| Drag the bar under a screen | Moves the screen; scroll while dragging to push it away or pull it closer. With the mouse, hold right while dragging to tilt it |
| Drag the tab on a screen's bottom right corner | Resizes the screen |
| Click the curve button (next to the bar) | Curves the screen around you, or flattens it |
| Drag the roll button sideways, or scroll on it | Rolls the screen; it snaps level near straight |
| While carrying a screen, sweep its laser across your other controller's ring, then let go | Pins it to that wrist, at its size and distance, as you hold it when you let go; it shows while you see its front. Grab its bar to adjust it (it stays pinned); sweep across the ring again to take it off |
| Set a screen to On your head (Frametop Display Settings, Visibility & pins) | Pins it to your head where it is, like a HUD. Grab its bar to move it; it stays on your head |
| Meta+Shift+R in the desktop | Puts the screens back in their layout (also in the menu as Reset Screen Layout, and mappable to a mouse button) |
| Meta+Alt+Tab, or Meta+Alt+Shift+Tab | Spins every screen and floating window around you together, like a lazy susan, so the next one on your right (or left) glides to straight ahead, with the pointer and typing going to it. Their arrangement stays the same: the room turns instead of you. Tap again to keep going; Meta+Shift+R puts the screens back. Pinned screens stay where they are |
| Meta+Shift+H in the desktop | Hides or shows all screens (also in the menu as Hide/Show Screens, and mappable). The Visibility & pins tab of Frametop Display Settings can instead show them only with the dashboard open, or while you look at your wrist |
| Tap Meta, on any keyboard | Opens the Steam menu in the SteamVR dashboard, or closes the dashboard, wherever you are. The desktop's launcher is still on the taskbar and Alt+F1. Change it in Frametop Input Settings (Keyboard page), where any key combination or modifier tap can do a Frametop or Steam action, open a profile, or run a command of your own |
| Switch a screen to Hidden (Frametop Display Settings, Visibility & pins → Screens shown) | Hides just that screen until you switch it back, whatever the other visibility settings say; new windows that would open on it float instead |
| Play a VR game | Frametop pauses so the game gets the headset to itself (see [Pause for VR games](#pause-for-vr-games)): the screens hide and your controllers stay in the game. Click both thumbsticks together twice to bring Frametop back. With the automatic pause off, the screens still hide, and the SteamVR dashboard or Meta+Shift+H shows them; to keep them visible over games, change During VR games on the Visibility & pins tab |

Restarting the desktop (Restart desktop in Frametop Display Settings) closes its windows, but background work you started in it, such as servers, tmux sessions, or builds, keeps running.

### Mouse and controllers

| Do this | To get this |
| --- | --- |
| Move the mouse | The dot moves around you and snaps onto whatever panel it's over |
| Click, right-click, scroll | Acts on the panel under the dot |
| Pick up a controller | The controller gets its laser back; move the mouse to take over again |

You can map the mouse's extra buttons to actions such as Toggle SteamVR dashboard, Recenter pointer, or Head follow on/off on the Buttons page of Frametop Input Settings, and the Frame controllers' buttons on its Controllers page. Pointer speed, dot size, and the rest are on its Pointer page and take effect immediately. If a panel you only look at, such as a performance overlay that follows your view, keeps catching the dot, tick it (or its whole app) on the Ignored panels page, and the pointer passes through it. Head follow, which is experimental and off by default, makes the pointer come along when you turn your head: it stays put until your head turns past the leash angle, then glides back to its place in your view, and a leash of 0 keeps it fixed in your view. It's only lightly tested and not polished; tuning its settings, or improving how it feels, is open to anyone who wants to take it further.

### Floating windows

Any desktop window can float in VR as a panel of its own. It stays a window of the same desktop, so drag and drop and the clipboard work between floating windows and the screens, and it's still in the taskbar and Alt+Tab.

| Do this | To get this |
| --- | --- |
| Meta+Shift+F over a desktop window | Floats that window, or puts it back on its screen if it floats. It acts on the window under the pointer, or the active one if the pointer is over the wallpaper. Rebind it, or map it to a mouse or controller button, in Frametop Input Settings (Keyboard page, or Buttons and Controllers as Float window in VR) |
| Click the float button, left of Close in a window's title bar | The same. Float in VR is also in every window's menu (Alt+F3). Apps that draw their own title bar, like Chromium and Electron apps, don't have the button: use Meta+Shift+F |
| Right-click an app in the Application Launcher (or the taskbar) and pick Launch as Standalone | Starts the app with its window floating, where that app last floated, or in front of you the first time. From a terminal: `float/ft-float launch org.kde.dolphin`, or `float/ft-float run <command>` |
| Meta+scroll over a floating window | Scales it up or down |

### Profiles

A profile is a named setup: where the screens are, with their sizes and pins, which ones are hidden, and which apps are open and where their windows are, on a screen or floating.

| Do this | To get this |
| --- | --- |
| Save as profile… (Frametop Display Settings, Layout & profiles) | Saves the current setup under a name, or updates the profile you're in |
| Pick a profile under Arrangement and press Open profile | Switches to it: the screens move, open windows of its apps go to their places, and the apps that aren't open start. Nothing closes |
| Pick a profile under Start in profile, or run its entry (Frametop: NAME) from SteamVR's Launch a program list | The desktop starts in that profile, or switches to it if it's running. A profile can also go on a key combination, mouse button, or controller button in Frametop Input Settings |

### Pause for VR games

Frametop pauses while a VR game runs, so the game gets the headset's CPU and GPU, and comes back a few seconds after the game ends. Paused, the screens hide and the desktop nearly stops drawing, but its windows stay open. Gaze mode's eye tracking stops, and so do remote desktop and hand tracking if they run. The mouse works as a plain mouse in SteamVR.

| Do this | To get this |
| --- | --- |
| Click both thumbsticks together, twice | Pauses Frametop, or brings it back, in a game or not. You hear a short sound. The game sees the clicks too |
| Start a VR game | Frametop pauses, and comes back 5 seconds after the game ends. Bring it back during the game, and it stays on until that game ends |
| Map Pause/resume Frametop to a mouse button, key combination, or controller button | The same, from that button (Frametop Input Settings) |

The Game optimization page of Frametop Input Settings turns the automatic pause off, changes the gesture, closes the desktop instead of hiding it (more for the game, but its windows close), and turns the sound off. From a terminal: `input/ft-pause on`, `off`, or `status`.

### Gaze mode (experimental)

In gaze mode the pointer goes where you look, and the mouse or the keyboard does the last bit. The installer offers it (or run `gaze/run.sh install` later), and then our own eye tracker for it, which is more accurate than SteamVR's (or run `gaze/tracker/install.sh` later; it needs `sudo`). Gaze mode uses ours once it's installed, and SteamVR's until then. Turn it on and calibrate it on the Gaze page of Frametop Input Settings.

| Do this | To get this |
| --- | --- |
| Tap Meta+J, or Meta+K | A left or right click where you look |
| Hold Meta+J, turn your head onto what you meant, and let go | A click there. Held still for half a second, it becomes a press, and turning your head drags |
| Hold the left mouse button, move the mouse onto what you meant, and let go | A click there. The right button does the same for a right click. Held still, the left button drags |
| Double right click (or double Meta+K) while dragging a screen's bar | Pans and tilts the screen |
| Put the headset on | A quick check: look at the dot for a moment, and the pointer lines up again |

By default the mouse only corrects: while the gaze has the pointer, moving the mouse does nothing until you hold a button. Each correction before a click teaches the tracker where it was off. A correction past the learning limit (55 degrees by default, about half of what you can see) starts a quick check instead. Calibrate, on the Gaze page, runs a full calibration in a panel in front of you: look at each dot and click. Check headset fit shows how well the eye tracker sees your eyes. [gaze/README.md](gaze/README.md) has the details.

### Leave the headset on a stand and reach it remotely

To keep the Frame on and connected while you're not wearing it, for SSH, remote desktop, or anything else running on it, open the Power tab in Frametop Display Settings:

- Turn off when unused for: how long the headset can go unused before its displays turn off (Never by default). Unused means the headset and controllers haven't moved and no mouse, keyboard, or button was used. SteamVR normally turns the displays off when its proximity sensor says the headset came off, but a stand or mount that covers the sensor makes the headset seem worn, so the displays stay on all night. This setting doesn't depend on the sensor. Pick the headset up or use any input, and the displays come back on.
- Stay awake while plugged in: stops Steam from putting the Frame to sleep while it charges. By default Steam puts it to sleep after an hour without input, even on the charger, which ends remote sessions. This is Steam's own Settings → Power → When Plugged In and Idle setting, so the power button still puts the Frame to sleep, and Steam's battery setting still applies.

With the displays off, the headset keeps tracking and rendering, so it uses about as much power as in use. Leave it on a charger that keeps up with that: a USB-C PD charger, not a 5 V one.

## Known limitations

This is an early release, tested on one Steam Frame (SteamOS 0.4.5 build 20261007, SteamVR 2.18.2; before that SteamOS 0.3.0 build 20260922, SteamVR 2.17.10).

- A SteamOS or SteamVR update can break parts of it until Frametop catches up. After an update, run `cd ~/frametop && scripts/doctor.sh` in a terminal. It checks what Frametop needs from SteamOS, and says what changed since the versions you last marked as working and what to try. Once everything works, `scripts/doctor.sh --mark-good` records the versions. If something stops working, please report it.
- The first install downloads 1–2 GB for the build container and compiles everything on the headset, which takes several minutes.
- During a VR game, mapped controller buttons belong to the game, so they can't bring the screens up. The pause gesture still works: Frametop reads it without taking the thumbsticks from the game. With the automatic pause off, open the SteamVR dashboard, press Meta+Shift+H, or use a mapped mouse button instead.
- Flatscreen games aren't detected as games, so they don't pause Frametop by themselves: click both thumbsticks twice to pause it. If your controllers end up working the screens instead of the game, set Controllers on the screens to "Only with the SteamVR dashboard open" (Frametop Display Settings, Visibility & pins tab).
- Typing follows your last click. A controller click on a panel other than the screens (the dashboard, a Steam app) doesn't move typing there; click it with the mouse, or click a screen to bring typing back.
- The screens don't draw a mouse cursor of their own. The 3D mouse's dot or SteamVR's laser shows where you're pointing.
- Profiles reopen apps, not what the apps had open. Tabs, files, and folders are left to each app's own restore.
- Dragging something from one panel to another (a screen and a floating window) works, but the dragged item's icon doesn't show while the pointer is between panels.
- Gaze mode is only as good as its calibration, and that depends on how the headset sits on your face. If the pointer lands off after you adjust the headset, run Quick check or Calibrate on the Gaze page of Frametop Input Settings.
- On SteamVR's Settings page, the 3D mouse shows a laser beam and a larger hit dot, like a controller. SteamVR doesn't tell other programs where that page is (unlike Steam's pages, such as Library), so the mouse used to miss most of it: clicks went through to a desktop screen behind, and the dot disappeared. As a workaround, on that page only, the laser starts near your eye and SteamVR finds the page itself. See docs/design.md.
- Remote desktop over VNC (Frametop Remote Access in the app menu, or `./desktops.sh remote on`) needs Tailscale on the Frame. It shows the primary screen only. The app turns it on and off, shows the address, and shows, copies, or changes the VNC password. The password is made at random on the Frame and kept in `~/.config/frametop-remote` (only you can read it); VNC limits it to 8 characters, and the tailnet encrypts the connection. Turning it on in a desktop that started with it off takes a desktop restart. It costs almost nothing until a viewer connects; the picture then takes a few seconds to appear.
- The desktop has no blur behind panels and menus, and no window animations, so it leaves the headset's GPU to SteamVR. Turn them back on in the Frametop desktop's System Settings (Desktop Effects, and Animation speed under General Behavior); Frametop won't turn them off again.
- Turning the displays off on a stand only turns their backlight off. SteamVR has no way for other programs to put the headset in standby, so tracking and rendering keep running, and the headset draws nearly its full power.

## Reporting problems

In a terminal on the headset, run:

```
cd ~/frametop && scripts/report.sh
```

This writes `frametop-report-<date>.txt` with version numbers, service states, settings, and recent logs. Bluetooth addresses and the headset's serial number are masked. It takes up to half a minute, because it also checks gaze mode: it starts the gaze service for a moment to see whether the eye tracker sends. If gaze or its calibration doesn't work, run it while you wear the headset. `scripts/gaze-report.py` prints only the gaze part, with what looks wrong first. Then [open an issue](https://github.com/DeeJanuz/frametop/issues), describe what you did, what you expected, and what happened, and attach the file. Quick questions can go to [Discord](https://discord.gg/W3X9f7z3Bc) instead.

## Update

Run the same command again. It updates `~/frametop` to the latest of the version you have (or switches, if you pick the other one) and installs it:

```
curl -fsSL https://deejanuz.github.io/frametop/get.sh | bash
```

Or by hand: `cd ~/frametop && git pull && ./install.sh`.

## Uninstall

In a terminal on the headset, run:

```
curl -fsSL https://deejanuz.github.io/frametop/uninstall.sh | bash
```

It works in two steps, so it never takes away the keyboard, mouse, or desktop you're using while it runs:

1. It stops Frametop from starting. Launch a program → Desktop opens the stock desktop again, and the Native Desktop entry, Frametop's services, its SteamVR driver, and its menu entries are removed, along with the system files of our eye tracker and the Bluetooth fixes and the file capabilities of hand tracking's camera broker (those need your `sudo` password). Everything running now keeps running until you restart the headset, and it offers to restart it for you.
2. After the restart, run the same command again. It deletes the code in `~/frametop`, and asks whether to delete your settings, any eye or hand recordings, and the build container (1–2 GB) too.

To see what it would do without changing anything, add `-s -- --dry-run` after `bash`. If the code isn't in `~/frametop`, add `-s -- --dir <folder>`. From the repo, the same script is `./uninstall.sh`.

Don't delete `~/frametop` by hand before you uninstall and restart: the desktop and the input relay run from it, and without it Launch a program → Desktop no longer opens anything. If you've already deleted it, the command above still works, since it doesn't need the repo.

Unless you ask for them to go, your settings stay: `~/.config/frametop.conf`, `frametop-input.json` (button maps and key combinations), `frametop-layout.json` (the layout and profiles), `frametop-float.json`, and `frametop-remote/` in `~/.config`, the gaze calibration in `~/.local/state/frametop`, and the desktop's own Plasma setup in `~/.config/frametop`. A later install picks them up again.

## How it works

A Plasma session runs nested inside ft-screens (`screens/`), a small Wayland compositor. KWin opens one window per screen, ft-screens sets each window's size, and each frame goes to SteamVR as an overlay without being copied. An input relay (`input/`) keeps Bluetooth mice working in SteamVR and feeds the mouse to the 3D pointer, which drives a virtual SteamVR controller (`pointer/`). A power service (`power/`) turns the displays off while the headset isn't used. [docs/reference.md](docs/reference.md) covers each piece, and [docs/design.md](docs/design.md) explains the design and what we learned about SteamVR on the Frame. [docs/hazards.md](docs/hazards.md) lists known ways the input handling can go wrong.

| Folder | What it is |
| --- | --- |
| `get.sh` | The one-line installer: picks stable or experimental, clones or updates the repo, and runs `install.sh`; or installs a built release (`--release`). |
| `install.sh` | The one-step installer. Safe to re-run. |
| `uninstall.sh` | The uninstaller: run it, restart the headset, and run it again. It doesn't need the rest of the repo. |
| `desktops.sh` | Start, stop, and configure the desktop, and install the input relay. |
| `screens/` | ft-screens, the compositor (wlroots and OpenVR). |
| `session/` | The desktop session script and its config example. |
| `layout/` | ft-layout: where the screens float, and their sizes. |
| `float/` | Floating windows: ft-floatd and the KWin script that float a desktop window in VR. |
| `decoration/` | The desktop's window decoration: Breeze's look plus the float button. |
| `input/` | The input relay (Bluetooth mice and keyboards, button maps). |
| `pointer/` | The 3D mouse: SteamVR driver, helper service, and a probe tool. |
| `power/` | ft-powerd: turns the displays off while the headset isn't used. |
| `gaze/` | Gaze mode (experimental): the gaze service, its calibration panel, our own eye tracker, and the gaze probe. See [gaze/README.md](gaze/README.md). |
| `hands/` | Hand tracking (experimental, deferred: the installer doesn't offer it). See [hands/README.md](hands/README.md). |
| `display-settings/`, `input-settings/` | The two settings apps (Kirigami, Python). |
| `remote/` | Frametop Remote Access, the app that turns remote desktop over VNC on and off. |
| `setup/` | The build container and the Bluetooth fixes. See [setup/README.md](setup/README.md). |
| `scripts/` | Helpers the installers use. They run commands locally on the Frame, or over SSH from a PC. |

## Packaging

`pack/` builds Frametop as an OCI image: the toolchain, the native binaries, and the locked Python environment (via [uv](https://docs.astral.sh/uv/)), built and tested by GitHub Actions. It is the groundwork for installing Frametop without building anything on the headset. No installer uses it yet. For development, `./ft dev build` builds the image and `./ft dev test` runs the tests inside it. [pack/design.md](pack/design.md) explains why an image and what is still open, and [pack/README.md](pack/README.md) documents the image itself.

## Developing from a PC

The scripts also work from a Linux or WSL PC over SSH, which is easier for editing code. On the Frame they use the local checkout; on a PC they sync the repo to `~/dev/frametop` on the Frame and run there.

1. On the Frame, turn on developer mode, set a password with `passwd`, and enable SSH with `sudo systemctl enable --now sshd`. Add your public key to `~/.ssh/authorized_keys`. [deck-tailscale](https://github.com/tailscale-dev/deck-tailscale) lets you reach it from anywhere.
2. On the PC, add the Frame to `~/.ssh/config` as host `frame`, or set `FRAME_HOST`:

   ```
   Host frame
       HostName <the Frame's address>
       User steamos
       IdentityFile ~/.ssh/<your-key>
   ```

3. The Bluetooth fixes need `sudo` on the Frame. The installer asks for the password in your terminal (over `ssh -t`). To skip the question, or to install with no terminal, put it in `.env` at the repo root instead. It's gitignored and never synced:

   ```
   steamos_root_pwd="<password>"
   ```

Then run `./install.sh` from the PC. If SteamVR isn't running on the Frame, the services that need it start with it later. Daily use:

```
scripts/doctor.sh                  # is the Frame reachable and ready?
scripts/doctor.sh --mark-good      # and record the versions Frametop works with
scripts/sync.sh                    # copy the repo to ~/dev/frametop on the Frame
scripts/frame.sh '<cmd>'           # run in the dev container, in the Frame's copy
scripts/frame.sh -C <dir> '<cmd>'  # same, in a folder of the repo
scripts/frame.sh --host '<cmd>'    # run on the SteamOS host
```

The sync only goes one way. It makes the Frame's copy match your checkout, deleting files there that you've removed, and skips `.git`, `build/`, `.env`, and anything gitignored. Edit on the PC only, since the next sync overwrites changes made in `~/dev/frametop` on the Frame.

Programs built in the `dev` container link against its glibc, which is newer than the host's, so they run inside the container. The SteamVR driver is the exception and is built to run on the host (see `pointer/driver/build.sh`). [AGENTS.md](AGENTS.md) has the working rules, including what not to restart while someone is using the headset.

## License

MIT. See [LICENSE](LICENSE).
