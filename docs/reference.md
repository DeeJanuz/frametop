# Frametop reference

How each part of Frametop works, where its settings live, and the commands for running parts of it by hand. For why it's built this way, see [design.md](design.md).

## The desktop

From the headset, open Launch a program → Desktop. The installer replaces that launcher entry with Frametop's (`~/.local/share/applications/deckard-nested-desktop.desktop`), and `desktops.sh uninstall` gives the stock single-screen desktop back.

From a terminal, on the Frame or from a PC over SSH:

```
desktops.sh install        # the launcher's Desktop entry starts Frametop
desktops.sh uninstall      # back to the stock SteamOS desktop
desktops.sh start | stop | restart | status | log [lines]
```

`session/frametop-session.sh` runs the desktop. It starts ft-screens in the `dev` container (log: `/tmp/frametop-screens.log`), then KWin and Plasma on the host inside it. Only one desktop runs at a time. `desktops.sh start` runs it in its own systemd unit, `frametop-desktop`. It keeps its Plasma config in `~/.config/frametop` (and state in `~/.local/state/frametop`), separate from the stock desktop's. Apps started in it inherit that. With `SHARE_CONFIG=1` in `frametop.conf` (the installer asks; on by default with Home Manager), `session/config_links.py` links everything else from `~/.config` and `~/.local/state` in at each start, and apps keep their config, logins, and history. Only Plasma's and KWin's own files stay separate (`KEEP_CONFIG`, `KEEP_STATE`); a desktop-only copy in the way is moved to `.replaced/` there.

When the VR launcher starts the desktop, it inherits the Steam client's environment. The session script drops the client's runtime from it (`LD_LIBRARY_PATH`, the `STEAM_*` settings, and the Steam overlay's Vulkan layer), so apps in the desktop use the system's libraries, including its video codecs, just as they would after a normal login.

Settings are in two files, and Frametop Display Settings edits both. The screens (resolution, width in metres, scale, curve, which one has the taskbar) and their layout are in `~/.config/frametop-layout.json`. The backend, remote desktop, and pointer settings are in `~/.config/frametop.conf`; `session/frametop.conf.example` lists every key.

Restarting the desktop closes its windows. Before the unit stops, `session/keep-apps.sh` moves every program started in the desktop into a systemd scope of its own, so background work such as servers, tmux, and builds keeps running. An app that shuts down its own helper processes when its window closes will still lose them; run that kind of work outside the desktop, for example as a systemd user service.

## ft-screens, the compositor

`screens/compositor.c` is a small wlroots 0.20 compositor that hosts the nested KWin, and `screens/vr.cpp` is its SteamVR side. `screens/build.sh` builds it; the installer runs that for you.

Each KWin window is one screen. ft-screens sets its size with an `xdg_toplevel` configure and KWin resizes the output to match, live. Frames arrive as DMA-BUFs and go to SteamVR through OpenVR's `IVRIPCResourceManagerClient::ImportDmabuf`, with no copy and no size limit.

Every screen is an overlay named `frametop.screen.N` with four controls:

- `.bar` moves the screen. Drag it with any laser or with the 3D mouse, whose right-drag tilts. Scrolling while you drag pushes the screen away or pulls it closer, along the line from your head.
- `.curve` bends the screen into a cylinder around you, using your current distance as the radius, or makes it flat again.
- `.roll` rolls the screen when you drag it sideways, like a knob. It snaps level within 2.5°, and scrolling on it turns 5° per notch.
- `.resize`, the tab on the bottom right corner, sets the width. Screens go down to 15 cm wide.

The controls are sized from both the screen's width and its distance from you, follow the surface of a curved screen, and stay invisible until a laser or the 3D mouse's cursor lands on one or comes within about 1.5 times a button's size of it. While invisible they're still there, fully transparent, so SteamVR's laser can find them. They're translucent until a laser is on them, like SteamVR's own window controls.

To pin a screen to a wrist, carry it by its bar and sweep the laser across your other controller. A ring around that controller marks the target, and a dot shows where the laser passes. Crossing the ring arms the pin, and the ring and bar turn blue; crossing it again disarms it. When you let go while armed, the screen rides on that controller at the size, distance, and angle it had, so you can arm the pin first and then turn the screen the way you want. Grab a pinned screen's bar to adjust it; it goes back to the same wrist when you let go unless you disarm it. A pinned screen shows only while you're looking at its front, within the wrist angle, and fades out over the last 10°.

To pin a screen to your head, like a HUD, set it to On your head on the Visibility & pins tab of Frametop Display Settings (or `ft-layout pin N head`). It rides on the headset where it is at that moment, so place it first, and it shows whenever the screens do. Grab its bar to move it; it goes back on your head where you let go. Sweeping across a wrist ring while you carry it moves it to that wrist, and sweeping across again leaves it in the room. The 3D mouse's dot stays in the room, so a head-pinned screen moves away from it when you turn your head, unless head follow is on.

The Visibility & pins tab of Frametop Display Settings decides when the screens show:

- Always. Meta+Shift+H, the Hide/Show Screens menu entry, or a mapped mouse button hides them.
- Only while the SteamVR dashboard is open.
- While you look at a chosen controller (the wrist gesture).
- Only after you show them with the hotkey.

In the last three modes the hotkey shows the screens anyway. A screen can also be hidden on its own (Screens shown on the same tab, or `ft-layout hide N`): it stays hidden whatever the mode or the hotkey says, until it's shown again there. Windows on it stay put, and a new window that would open on it floats instead (ft-floatd). Profiles use this to show only some screens. Two more settings on the same tab cover VR games, which ft-screens detects as SteamVR scene apps:

- During VR games, the Always mode hides the screens unless the dashboard is open (the default), or leaves them up.
- Controllers on the screens. Visible screens can keep SteamVR's laser mouse on, so controllers work them with the dashboard closed, but that also takes the controllers away from a game. By default this is off while a VR game runs, and the 3D mouse or the dashboard works the screens. The other choices are always on, or only with the dashboard open, which also suits flatscreen games since they aren't scene apps.

Input from the lasers reaches KWin through ft-screens' own seat. Keys come from the input relay, from pass-through keyboards and any key a pointer device passes through. Typing follows your last click: after a click on a screen it goes to the desktop, even with the SteamVR dashboard open, and after a mouse click on any other panel (the dashboard, Steam, an app like Spotify) it goes there instead. While it goes to the desktop, the relay grabs pass-through keyboards so gamescope, which reads every keyboard itself, doesn't type them into the Steam app too. A program that watches every keyboard for a hotkey loses a grabbed one; with `SHARE_KEYS=1` in `~/.config/frametop.conf`, their keys also go to `@frametop_keys` for it. That's off by default, since any local process that binds the name first would get everything typed into the desktop. Hidden screens don't take typing.

Frametop's keyboard opens by itself when a text field on the desktop gets focus, and stays open until its Close key, a layout reset, or a mapped button closes it (or, with Keep it open off in Frametop Input Settings, until the text field loses focus). While the Steam menu (the dashboard) or Steam's own keyboard is up, it steps aside, and it comes back where it was when they're gone; one asked for meanwhile appears then. In the "only with the dashboard" visibility mode, the dashboard doesn't count. It doesn't open without a head pose (the headset in standby). It's a panel of keys (a US laptop layout, with Esc where Caps Lock would be, arrows, and a Close key) that ft-screens shows 0.7 m in front of you and below your eyes, facing you. It stays where it opened, and its grab bar (the pill along the top) moves it like a screen's. Type on it with a controller's laser or the 3D mouse. Shift, Ctrl and Alt latch for the next key, and a held key repeats. KWin starts `input/ft-textinput` as the desktop's input method, and KWin activates it whenever the focused app turns on text input for a field. It tells the relay (`textfield 1` or `0`), the relay decides by the Keyboard setting in Frametop Input Settings, and ft-screens opens the keyboard for the screen that has keyboard focus (`vrkeyboard show`, `hide`, or `toggle` from a mapped button). Its keys reach the focused screen as key presses, so it works in every app, but only apps that use Wayland text input (Qt, GTK, Firefox) open it by themselves; Chromium, Electron and X11 apps need the button. The session drops the `QT_IM_MODULE=xim` and `GTK_IM_MODULE=xim` that the gamescope session sets, or Qt and GTK apps wouldn't use Wayland text input either.

KWin's nested backend doesn't undo a screen's scale on pointer input, so ft-screens divides panel positions (in pixels) by it. `ft-layout` sends it each screen's scale as KWin reports it (`scale N s`) whenever it applies scales: at desktop start and from Frametop Display Settings. A scale changed only in Plasma's own display settings is put back to the Frametop layout's the next time `ft-layout` runs.

ft-screens listens for datagrams on the abstract socket `@ft_screens` and replies to the sender:

```
place N x y z yaw pitch roll     width N metres          curve N radius|on|off
pin N|all left|right|head [matrix]    unpin N|all        size N w h
get N    screens    head    state    key code value    scale N s    vrkeyboard show|hide|toggle|close
visibility always|dashboard|gesture|toggle    wrist degrees    gesture left|right degrees
hide | show | toggle    controllers always|outside_games|dashboard    ingames hide|visible
conceal N|all    reveal N|all    concealed    cutouts on|off|state    cutouts predict on|off    cutouts lead ms
float N mpp x y w h title    unfloat N    pose N matrix    sub N k x y w h | sub N k off    minimized N 0|1    carry N
```

`conceal` and `reveal` hide and show one screen on its own (`ft-layout hide` and `show` send them), and `concealed` lists those screens. `cutouts` turns the hand cutouts on and off (`ft-handsctl cutouts`). The last line is ft-floatd's, for floating windows: N is a floating window's panel, numbered on from the screens, one per spare output. `float` gives the window's rectangle in its output, metres per pixel, and the title bar's height, and shows the panel; `unfloat` hides it. `pose` places it (a 3x4 matrix, standing universe), `sub` shows popup or dialog k over it, `minimized` hides it while its window is minimized, and `carry` moves it with the laser that pressed the window's own title bar.

## Input relay

SteamVR opens input devices only when it starts. A Bluetooth mouse that sleeps and reconnects gets new device nodes, SteamVR keeps reading the dead ones, and the mouse stops working until SteamVR restarts. `input/input-relay.py` avoids this. It creates two virtual devices, `frametop virtual mouse` and `frametop virtual keyboard`, through `/dev/uinput` before SteamVR starts. It then grabs USB and Bluetooth mice and keyboards as they come and go and forwards their events, so SteamVR only ever sees the virtual devices, which never go away.

It runs as the user service `frametop-input-relay.service`, ordered before `steamvr.service`.

The relay also owns the volume keys, on every device that has them, the headset's buttons included. It changes the volume itself (`wpctl`, 5% a step, repeating while held), and nothing else sees a volume key, gamescope and SteamVR included: on devices with a keymap (the headset's `gpio-keys`, USB and Bluetooth keyboards) it remaps just the volume entries to unused codes (`KEY_MACRO29`, `KEY_MACRO30`), so the headset's click button and the other keys still work, and it grabs `pmic_resin`, which has only volume down. The keymaps go back when the relay stops. With `--no-grab` it leaves the volume keys alone.

```
desktops.sh relay install     # enable it (starts with the next reboot or SteamVR start)
desktops.sh relay status | log | uninstall
input/input-relay.py --no-grab   # try it without taking devices from SteamVR
```

The first time, the relay has to start before SteamVR, so reboot or restart SteamVR after installing it. After that it's safe to restart on its own: systemd keeps the virtual devices open in its file descriptor store (`FileDescriptorStorePreserve=yes`), so SteamVR keeps the same devices.

## The 3D mouse

A mouse drives SteamVR the way a controller's laser does, but shows up as a small dot anchored in the room. It snaps onto panels and works on the dashboard, Steam, overlays, and the desktop. Three pieces make it work:

- The input relay, in pointer mode (`POINTER=1`), sends mouse motion, clicks, and scrolling to the helper. A deliberate movement or a click wakes the pointer, and 30 seconds without mouse input releases it.
- The helper, `pointer/helper/ft-pointer`, runs in the `dev` container as `frametop-pointer.service` and starts with SteamVR. It keeps the cursor, tests it against every visible overlay, draws the dot, and sends the driver an exact pose.
- The driver, `pointer/driver/` (`ft_pointer`), is loaded by SteamVR. It's an invisible virtual right-hand controller whose laser follows the cursor.

Whichever device you used last wins. Picking up a controller hands the laser back at once, and moving the mouse takes it again. When the headset comes off, the pointer lets go, so the displays can sleep, and it stays off until you're wearing the headset again.

To move a floating panel, left-drag its grab bar. The scroll wheel pushes and pulls it while you drag. Hold the right button while dragging and move the mouse to tilt the panel around the grab point; the right press isn't sent as a click. The tilt stays for the rest of the drag, and releasing the left button drops the panel as it is. A mapped Toggle dashboard button (or a Meta tap, with `META_DASHBOARD=1`) wakes the pointer if needed and holds the virtual system button for 0.12 s, because SteamVR ignores a press and release in the same instant.

```
pointer/driver/build.sh && pointer/driver/install.sh install   # then restart SteamVR
pointer/helper/build.sh && pointer/helper/run.sh install
pointer/helper/run.sh status | log | restart
pointer/driver/install.sh probe     # devices, hand roles, who owns the dashboard pointer
```

The pointer settings are in `~/.config/frametop.conf`: `POINTER_SENSITIVITY`, `POINTER_IDLE`, `POINTER_WAKE_COUNTS`, `POINTER_CONTROLLER_PICKUP`, `POINTER_DISTANCE`, `POINTER_CURSOR_DEG`, `POINTER_ORIGIN_FRACTION`, `POINTER_ORIGIN_MARGIN`, `POINTER_SCENE_RADIUS`, `POINTER_EDGE_REACH`, `POINTER_LASER_WIDTH`, `POINTER_IGNORE`, the head follow settings `POINTER_FOLLOW`, `POINTER_LEASH_DEG`, `POINTER_LEASH_DELAY`, `POINTER_LEASH_RETURN`, and `POINTER_FOLLOW_REACH`, and the gaze mode settings `POINTER_GAZE`, `POINTER_GAZE_RETAKE`, `POINTER_GAZE_NUDGE_MAX`, `POINTER_GAZE_HOLD`, `POINTER_GAZE_DOT`, `POINTER_GAZE_SHOW`, `POINTER_GAZE_MOUSE`, `POINTER_GAZE_MOUSE_MOVE`, and the keyboard clicks' `POINTER_HEAD_DEADZONE` and `POINTER_KEY_TAP`, and the gaze service's `GAZE_TRACKER` (SteamVR's eye tracker or our own) and `GAZE_EYE` (the eye bias). The example config explains each. Frametop Input Settings changes them live; after editing the file by hand, restart the relay or the helper (the gaze service reads its two again when the file changes).

## Frametop Input Settings

A Kirigami app with a Python backend, in the Plasma menu under Settings. It runs in the `dev` container and talks to the relay over its control socket, `@frametop_relay`. It has eight pages:

- Devices lists every USB and Bluetooth mouse and keyboard, with a light that flashes when the device is used. Each device gets a role: 3D pointer (grabbed, drives the pointer; the default for anything with a mouse), Pass through (grabbed only while typing goes to the desktop; the default for keyboards, where a Meta tap toggles the dashboard if `META_DASHBOARD=1` is in `~/.config/frametop.conf`), or Ignore. A device is identified by its Bluetooth address, or its USB ids and name, so all of its input nodes share one role. Forget drops everything saved for a device.
- Buttons maps a pointer device's buttons. Choose Capture a button, press the button or key, then pick an action: a click, back, scroll, toggle dashboard, recenter, pointer on or off, head follow on or off, gaze pointer on or off, gaze precision, gaze drag, gaze quick check, faster or slower, reset the screen layout, hide or show the screens, open or close the keyboard, float a window in VR or put it back, put all floating windows back, Open profile NAME (one per profile, [profiles.md](profiles.md)), pass the key through, or nothing. Devices with saved mappings are listed even while they're asleep.
- Controllers maps the Frame controllers' buttons (every button but the system button) to the same actions, except passing a key through and the gaze actions: gaze mode is a mouse and keyboard feature ([gaze-controllers.md](gaze-controllers.md)). Capture a button and press it on a controller, or pick it from the list. The controllers aren't input devices on the host; only SteamVR sees them. So the pointer helper reads them with SteamVR input (`pointer/helper/vrbuttons.h`, `pointer/helper/actions/`) and sends presses to the relay (`vrbtn right/a 1`), which does the mapped action. The helper only takes the buttons that are mapped (the relay tells it with `vrbind`), at an overlay-global priority, and only while no game (scene application) runs, so games keep every button; with In games on (`controller_in_games`), a mapped button is taken from games too. That needs SteamVR's "Enable global input from overlays (Experimental)" setting (`steamvr/globalActionSetPriority`), which the page's Global input switch turns on and off. Mappings are saved as `controller_buttons` in `~/.config/frametop-input.json`.
- Keyboard sets when Frametop's keyboard opens: whenever a text field is selected; only while no pass-through keyboard is connected (the default; keyboards other programs make through uinput, like frame-voice's, don't count); only with a mouse or controller button mapped to Open/close keyboard; or never, which turns the button off too. Keep it open (on by default, `vr_keyboard_persist`) leaves it open after the text field loses focus. The mode is saved as `vr_keyboard` in `~/.config/frametop-input.json`, and the page lists the keyboards that count as connected. Its Key combinations section maps modifiers plus a key, on any keyboard, to any action but passing a key through or nothing. The gaze clicks (Gaze left click and Gaze right click) only go on key combinations. The defaults are Meta+J (gaze left click), Meta+K (gaze right click), and Meta+Shift+F (float window in VR or put it back); remove them or add others there. The combination's last key isn't typed, and the modifiers still reach the app. They're saved as `key_bindings` in the same file.
- Pointer has a Head follow switch and sliders for the pointer settings, which apply immediately, and a Recenter button.
- Ignored panels lists the SteamVR overlays that are showing, grouped by app (the first two parts of the overlay key, such as `sasaken.frame-perf-overlay`), from the pointer helper (`overlays`). Tick a panel, or Ignore the whole app, and the pointer passes through it to what's behind. It's for panels you only look at, like a performance overlay that follows your view. The list is saved as `POINTER_IGNORE` in `~/.config/frametop.conf`: comma-separated overlay keys, where a shell pattern like `vendor.app*` covers a whole app, including panels it opens later. The helper reloads at once. Frametop's own screens aren't listed, and entries for apps that aren't open are listed below, to remove.
- Gaze has the gaze pointer switch (on now and from now on; a mapped button toggles it until the helper restarts), what the mouse's left button and movement do, the gaze dot, the eye tracker and eye bias, the gaze mode sliders, the gaze service's state (headset, samples per second, how often the tracker is losing each eye, the calibration, the nudges learned), and Quick check, Calibrate, and Check headset fit (each in a panel in the headset), Reload calibration, and Forget nudges. The gaze probe, a development tool, is in the page's overflow menu.
- Bluetooth lists paired devices and has Apply Bluetooth fixes, which runs `/etc/steamframe/bt-fixups.sh` through `pkexec`. Pair new devices in Steam.

Device rules are saved in `~/.config/frametop-input.json`. `input-settings/install.sh` installs the menu entry. Its launcher hands podman the real `XDG_RUNTIME_DIR` and user bus and gives the app the session's Wayland socket, because the desktop session runs on a private D-Bus and podman fails on it.

## Frametop Display Settings and ft-layout

When the desktop starts, its screens arrange themselves around where you're facing. You can move them by hand at any time and put them back with Meta+Shift+R, the Reset Screen Layout menu entry, Arrange now in the app, or a mouse button mapped to Reset desktop screen layout.

The desktop's own screen arrangement follows where the screens are around you, whatever their numbers: a screen you see to the left of another is to its left in Plasma too, so the pointer and dragged windows cross straight to it. Screens one above the other stack, and screens pinned to a wrist or your head come last. It's updated at startup, after arranging or saving the layout, and half a second after you let go of a screen you moved. With the headset off there's no head pose to go by, and the arrangement stays as it was.

Frametop Display Settings has four tabs (three with the gamescope backend, which has no Visibility & pins):

- Screens: add and remove screens, and set each one's resolution (presets from 1080p to 4K, ultrawide, super ultrawide, portrait, or custom), its width in VR (0.5 to 6 m), its scale, whether it's curved, and whether it has the taskbar. Resolution, width, and curve apply at once. Adding or removing a screen takes a desktop restart, which the app offers.
- Layout (the Layout & profiles page): the Arrangement list starts with two presets: Curved around you, with the screens hinged edge to edge like monitors on a desk and each turned to face you, and Flat wall. Both take rows, distance, gap, and height, and Arrange now applies them. Save as profile… saves where the screens are now (positions, sizes, curves, and pins), which ones are hidden, and the open apps and where their windows are, under a name. Profiles are listed in Arrangement after the presets: pick one and Open profile switches to it, and the buttons next to the list rename or delete it. Below it, Apps lists the profile's windows and where they go, and Leave out drops one. A profile saved with fewer screens than you have now leaves the others where they were saved last, or where the preset would put them. Start in profile picks the profile the desktop starts in; with None, a switch turns auto-arrange at startup on or off. A preview shows the layout from above and from the front. See [profiles.md](profiles.md).
- Visibility & pins: the visibility, game, and controller settings described above, the wrist angle, where each screen is pinned (in the room, a wrist, or your head), and buttons to pin all screens or unpin them.
- Power: when the displays turn off while the headset isn't used, their state now, Turn displays off now (to try it), and Stay awake while plugged in. See [Displays off and sleep](#displays-off-and-sleep).

`layout/ft-layout` does the arranging. It's a Python script that uses only the standard library and runs on the host:

```
layout/ft-layout apply      # arrange every screen
layout/ft-layout capture    # save the current arrangement and sizes as the layout
layout/ft-layout save NAME  # ...under a name too, with the open apps and hidden screens (a profile, docs/profiles.md), and use it
layout/ft-layout use NAME   # switch to a profile: arrange the screens in it and open its apps
layout/ft-layout open NAME  # a profile's launcher entry: use it, or start the desktop in it
layout/ft-layout default NAME|none  # the profile the desktop starts with (start --wait runs it at desktop start)
layout/ft-layout layouts    # list the named layouts (* = in use); rename OLD NEW, delete NAME
layout/ft-layout pin N|all left|right|head   # pin as they are now; unpin N|all
layout/ft-layout plan       # print the arrangement as JSON (no VR needed)
layout/ft-layout scale      # per-screen scale, positions (as the screens are around you), and taskbar screen, to KWin
layout/ft-layout toggle     # hide or show all screens
layout/ft-layout hide N|all # hide a screen on its own, whatever the visibility mode; show N|all brings it back, hidden lists them
display-settings/install.sh # menu entries and the Meta+Shift+R and Meta+Shift+H shortcuts
```

The layout is stored relative to your head when it's applied. `/tmp/frametop-layout.log` has the run from the last desktop start.

## Floating windows

A desktop window can float in VR as a panel of its own, away from the screens. Meta+Shift+F floats the window under the pointer (or the active one, over the wallpaper), or puts it back on its screen if it floats. So do Float in VR in every window's menu (Alt+F3; Back to Desktop on a floating one), the button left of Close in its title bar, and a mouse button, controller button, or key combination mapped to Float window in VR in Frametop Input Settings; Put all floating windows back is mappable too. Launch as Standalone, in an app's right-click menu in the Application Launcher or the taskbar, starts the app with its first window floating, where that app last floated or in front of you. [floating-windows.md](floating-windows.md) explains how it works.

`float/ft-floatd` does this. It runs inside the desktop's Plasma session (log: `/tmp/frametop-floatd.log`), loads the KWin script `float/frametop-float.js`, and moves each floating window to a spare KWin output of its own, which ft-screens shows as a panel cropped to the window. The settings are in `~/.config/frametop.conf`: `FLOAT_SLOTS` is how many windows can float at once (8, at most 16; 0 turns floating off; restart the desktop after a change), and `FLOAT_MARGIN` the pixels around each window on its output, so menus have room past its edges (300). Each app's last floating place, size, and scale are kept in `~/.config/frametop-float.json` by desktop file name, relative to the primary screen, so they move with the screens. `FT_FLOAT_DEBUG=1` in ft-floatd's environment logs every event from the KWin script.

With floating on, the session gives the desktop's windows Frametop's own decoration (`decoration/`): Breeze's look plus the float button. Apps that draw their own title bar, like Chromium and Electron apps, don't have the button. `decoration/apply.sh` puts a changed copy into the running desktop, and `decoration/apply.sh --off` goes back to Breeze until the next desktop start.

```
float/ft-float float [ID|active]      # float a window (default: the active one, as a toggle)
float/ft-float float pointer          # the float key: the window under the pointer, floated or put back
float/ft-float dock [ID|active|all]   # put a floating window back (default: the active one), or all of them
float/ft-float launch org.kde.dolphin # start an app (its desktop file name) floating
float/ft-float run COMMAND [ARG...]   # the same for a command
float/ft-float close ID               # close a window
float/ft-float list                   # the spare outputs and what floats on them
```

## Displays off and sleep

SteamVR turns the displays off a few seconds after the headset's proximity sensor says it came off. A stand or display mount that covers the sensor makes the headset seem worn, so its displays stay on, and Steam, which then counts someone as present, never puts it to sleep either.

`power/ft-powerd` goes by use instead. It runs in the `dev` container as `frametop-power.service` and starts with SteamVR. Once the headset has gone unused for `DISPLAY_OFF_MIN` minutes (0, the default, is never), it turns the displays' backlight off, and it turns it back on at the next use. Use is any of these:

- The headset, a Frame controller, or the 3D mouse's virtual controller moving more than `DISPLAY_MOVE_MM` (5 mm) or turning more than `DISPLAY_MOVE_DEG` (0.5 degrees) within 10 seconds.
- A key, button, or mouse motion on any input device on the host, including the headset's own buttons and the input relay's virtual mouse and keyboard.
- The headset going back on after SteamVR's own standby, or something else turning the backlight back on.

While SteamVR has the headset in standby, SteamVR owns the displays and ft-powerd waits. The backlight is `/sys/class/backlight/ae94000.dsi.0/brightness`, the same file SteamVR's driver writes for standby. With the backlight off, tracking and rendering keep running, which lets the displays wake the moment the headset moves, but the headset still uses most of its power. ft-powerd puts the backlight back when it stops, and if it was killed with the displays off, the next start does (the value is kept in `~/.cache/frametop/powerd-brightness` meanwhile).

Stay awake while plugged in is Steam's own setting, When Plugged In and Idle → Sleep after (`system_idle_suspend_ac_sec`), set to Never. Frametop Display Settings changes it the way Steam's Settings → Power page does, through Steam's UI on its debugging port (`display-settings/steam_settings.py`), and keeps the value from before in `STEAM_SLEEP_AC_BEFORE` to put back when the switch goes off. The power button still puts the Frame to sleep, and Steam's battery setting still applies.

```
power/build.sh && power/run.sh install
power/run.sh status        # "ok on|off|away <seconds unused> <timeout seconds>"
power/run.sh off | on      # the displays off now, or back on
power/run.sh log
```

## Gaze pointer (experimental)

In gaze mode the 3D mouse's pointer goes where you look, and the mouse or the keyboard does the last bit. It needs the gaze service, which `install.sh` offers (yes by default) and `gaze/run.sh install` installs on its own: it builds it and runs `gaze/ft-gazed` as `frametop-gaze.service`, which starts with SteamVR. Turn gaze mode on with the Gaze page of Frametop Input Settings, `gaze/ft-gazectl on`, `POINTER_GAZE=1`, or a button or key combination mapped to Gaze pointer on/off.

- Meta+J left-clicks and Meta+K right-clicks where you look. A quick tap clicks where the dot was at the press. Hold instead, and the dot stays put in your view: turn your head until it's on what you meant, and let go to click there. Held still for `POINTER_GAZE_HOLD` (0.5 s), the press becomes a real one, and your head drags. Meta+K with Meta+J held presses where the dot is now, to drag from there, and a second Meta+K during that drag (a double Meta+K) pans and tilts what you're dragging while it's held.
- The mouse's buttons work the same way, with the mouse steering instead of your head (`POINTER_GAZE_MOUSE=precision`, the default): the right button with the left held starts a drag, and a double right click pans and tilts what you're dragging. With `POINTER_GAZE_MOUSE_MOVE=held`, the default, the mouse only corrects: while the gaze has the pointer, moving it does nothing unless a button is held. `free` lets the mouse take the pointer any time. With the gaze stale for a second, in a game, or with the headset off, the mouse works as usual.
- A correction before a click teaches the gaze service the tracker's error there. A correction bigger than `POINTER_GAZE_NUDGE_MAX` (55 degrees) isn't learned; it opens a quick check instead.
- Calibration and checks run in a panel fixed to the headset (`gaze/panel/ft-gazepanel`, which the gaze service runs), from the Gaze page: Quick check is one dot, and also opens when you put the headset on. Calibrate is three rounds of dots, dark to bright; look at each dot and left click or press Meta+J to take it. Check headset fit shows, live, how well the tracker sees each eye. A right click or Meta+K closes the panel. Gaze mode coming on without a calibration opens Calibrate by itself.

[gaze/README.md](../gaze/README.md) has the details, our own eye tracker, and the gaze probe, a development tool.

## Hand tracking (experimental, deferred)

Deferred: it costs a lot of the headset's CPU and needs more work, so `install.sh` doesn't offer it. It still builds and runs, installed by hand, for working on it.

Your hands show over the screens: where a tracked hand is between an eye and a screen, ft-screens lets that eye see the room through the screen. The same tracker detects pinches and grips, and with `POINTER_HANDS=1` in `~/.config/frametop.conf` they work the pointer. In gaze mode a pinch clicks where you look when it opens; hold it and move the hand to correct the pointer first. Without gaze mode a pinch is a press like the mouse's button, so a held pinch drags. A grip (closing the hand) presses and drags. To install it: `hands/run.sh install`.

- `ft-camd` borrows XRService's camera buffers and publishes the four IR tracking cameras to `/run/user/UID/frametop-hands/cam-ring`. It runs on the host as `frametop-camd.service`, with file capabilities that `hands/run.sh install` sets through sudo, and it drops them once set up. A rebuild clears them: `hands/run.sh caps`.
- `ft-hands` runs in the `dev` container as `frametop-hands.service`. It finds and triangulates the hands, and publishes `hands` (read by ft-screens' cutouts) and `gestures` (pinches and grips, read by the pointer helper) next to the ring.
- The install leaves both off, and they don't start with SteamVR. `ft-handsctl on` starts them while SteamVR runs, and `ft-handsctl off` stops them; they also stop with SteamVR. The install links `ft-handsctl` into `~/.local/bin`. `ft-handsctl status` and `ft-handsctl log` (or `hands/run.sh status` and `log`) show how they're doing, `ft-handsctl cutouts on|off` turns just the cutouts off, and `ft-handsctl gestures` shows pinches and grips live.
- Settings in `~/.config/frametop.conf`: `HANDS_SWAP_SIDES` (after some SteamVR restarts the side cameras' names come out swapped, and hands land beside the holes; `hands/tools/check_sides.py --ring` tells), `HANDS_CPUS`, the cameras it tracks with (`HANDS_CAMERAS`, `HANDS_BRIGHT`, `HANDS_BRIGHT_ON`, `HANDS_BRIGHT_OFF`, `HANDS_COLOR_LEFT`, `HANDS_COLOR_CROP`), and the pointer helper's `POINTER_HANDS`, `POINTER_PINCH_GAIN`, `POINTER_PINCH_DEADZONE`, `POINTER_GRIP_GAIN`, `POINTER_GRIP_BELOW`, and `POINTER_PINCH_TYPING`. The example config explains each.

Details, options, and the recording and replay tools are in [hands/README.md](../hands/README.md).

## Remote desktop over VNC

With `REMOTE=1` in the config (`desktops.sh remote on`), the desktop's primary screen (the one with the taskbar) is also served over VNC, at that screen's resolution, for RealVNC Viewer or macOS Screen Sharing. `desktops.sh remote info` prints the address and password.

It listens on port 5900 on the Frame's Tailscale address only, not the LAN, so it needs Tailscale on the Frame ([deck-tailscale](https://github.com/tailscale-dev/deck-tailscale)). VNC authentication has no encryption of its own, so viewers warn about it, but the tailnet encrypts the traffic. The password is in `~/.config/frametop-remote/vnc-password` and VNC limits it to 8 characters. To change it, delete that folder and restart the desktop.

No VNC server can capture KWin on SteamOS directly: `krfb` needs `xdg-desktop-portal-kde`, which SteamOS doesn't ship, and `wayvnc` only works with wlroots compositors. So `session/remote-desktop.sh` captures the desktop with KDE's `krdpserver --plasma` on `127.0.0.1:3390`, and `session/vnc-bridge.sh` runs TigerVNC's `Xvnc` on display `:20` with a FreeRDP client inside it and serves that. Both run in the `dev` container, and the extra hop adds a little latency. krdp streams every screen; the VNC screen is the primary's size, and the FreeRDP window is shifted so the primary fills it (`ft-layout remote-view` gives the offset). krdp's own `--monitor` would stream just one screen, but it maps the pointer as if that screen sat at 0,0, so clicks would miss. When the layout changes, the VNC screen resizes and FreeRDP reconnects within a few seconds.

With remote access on, the nested KWin runs with `KWIN_WAYLAND_NO_PERMISSION_CHECKS=1` and `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1`, so any app in the Frametop desktop could capture its screens or inject input. The second one lets scripts take screenshots through KWin's `org.kde.KWin.ScreenShot2` D-Bus interface. This applies only to that desktop, not the stock one. Port 3389 is SteamOS's own `xrdp`, which starts a separate X11 session rather than showing the VR desktop.

## Limits

- A controller button can't show hidden screens; a mapped mouse or keyboard button can.
- KWin's cursor isn't drawn on the screens, because KWin draws it as a host cursor, which ft-screens doesn't render. The 3D mouse's dot and SteamVR's laser dot show where you're pointing.
- The old gamescope backend (`BACKEND=gamescope`) still works, but it gives every screen the same resolution, at most 1920×1080 pixels' worth, and arranging screens borrows the pointer for a few seconds.
