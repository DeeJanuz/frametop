# Profiles

Profiles are built: Display Settings, `ft-layout`, each profile's launcher entry, and the input relay's `profile:NAME` action open them, and the desktop can start in one.

A profile is a named layout that also opens apps. It holds:

- where each screen goes, with its size in metres, curve, roll, and pin (what a named layout held before profiles);
- which screens show and which are hidden;
- the apps, one entry per window: on a screen at a place and size, or floating at a pose, size, and scale;
- the remote displays connected when it was saved, each with its place and whether it's hidden ([remote-displays.md](remote-displays.md)). Opening the profile connects them, if their computer answers, and puts them back. Like its apps, it leaves other displays connected.

So a "Work" profile can put three screens around you with a browser, two terminals, and an editor on them, and a "Couch" profile can hide every screen and float one video player in front of you.

## Decisions

| # | Question | Decision |
|---|---|---|
| 1 | Profiles and named layouts | One list. Named layouts grow into profiles; a layout saved before profiles existed is a profile with no apps and every screen shown |
| 2 | Making one | Capture what's open: the screens and every app's windows. Display Settings lists a profile's apps, so one can be removed. No editor beyond that |
| 3 | Apps with several windows | One entry per window. The app is launched once; when its first window shows up, it's launched again for each window still missing. A browser that restores its own windows gets launched once, a terminal twice |
| 4 | What's recorded of an app | Its desktop file name, or its command line if it has none. Not what it had open: tabs, files, and folders are left to the app's own restore |
| 5 | Switching while apps are open | Additive: launch what's missing, move the windows that match into place, leave the rest alone. Nothing is ever closed |
| 6 | Saving changes | Only on an explicit save. Moving things after switching doesn't change the profile |
| 7 | Starting one | Four ways: a default profile when the desktop starts, Display Settings, a launcher entry for each profile, and a mappable action |
| 8 | Plasma's session restore | Off in the Frametop session, so a profile is the only thing that reopens apps |
| 9 | Screen count and resolution | Not part of a profile. They're global, because changing them restarts the desktop, which closes every window |

## Where profiles live

`~/.config/frametop-layout.json` keeps its `layouts` as they are (`{"Work": [screen places]}`), so older copies of ft-layout still read it. What a profile adds goes in a parallel `profiles` map under the same names:

```
"layouts":  {"Work": [{"pos": ..., "face": ..., "roll": ..., "metres": ..., "curve": ..., "pin": ...}, ...]},
"profiles": {"Work": {"hidden": [3],
                      "windows": [
                        {"app": "org.kde.konsole", "screen": 2, "rect": [40, 60, 1200, 800], "maximized": false},
                        {"app": "com.brave.Browser", "screen": 1, "maximized": true},
                        {"cmd": ["/opt/tool/run"], "class": "tool", "screen": 1, "rect": [...]},
                        {"app": "org.kde.dolphin", "float": {"rel": [12 numbers], "pixels": [1400, 900],
                                                             "scale": 1.2, "mpp": 0.00097}}]}},
"default_profile": "Work"
```

- `screen` is 1-based, as everywhere in Frametop. `rect` is the window's frame in KWin's logical units, relative to its screen's output, so it survives the screens being arranged differently.
- A floating window's place (`rel`) is its panel's centre and axes in the frame of the primary screen's panel, the same way ft-floatd remembers each app's place. The screens go relative to your head when the profile is applied, and the floating windows follow them. `mpp` is its density in metres per pixel, and `scale` its scale, put back with the rest.
- Renaming or deleting a layout renames or deletes its profile entry with it.

## How it works

- **Capture** (`ft-layout save NAME`, and Save as profile… in Display Settings). ft-layout captures the screens as before, then asks ft-floatd for the windows (`windows` on @frametop_float). ft-floatd has the KWin script report every window as it is now (`report-all`), then answers with every normal window: its desktop file name, the screen it's on, its rectangle there, and whether it's maximized. For floating windows it gives their panel's place, their size in pixels, and their scale. A window whose app id has no desktop file (a Flatpak app's X11 window can give its own: RustDesk's says `com.carriez.flutter_hbb`, its desktop file is `com.rustdesk.RustDesk`) is kept by the desktop file whose `StartupWMClass` names its window class. Windows with no desktop file name are kept by their process's command line (`/proc/<pid>/cmdline`) and window class. Windows of Plasma itself, the Frametop settings apps, and dialogs aren't recorded. If ft-floatd doesn't answer, the profile keeps the apps it had.
- **Apply** (`ft-layout use NAME`, Open profile in Display Settings). ft-layout makes the profile's hidden screens the screens' own setting, arranges the screens (which hides and shows them: ft-screens' `conceal` and `reveal`), then has ft-floatd open the apps (`profile NAME`; ft-floatd reads the windows from the layout file). If the screens can't be arranged, for example with the headset off and no head pose, the apps still open: the screens stay where they are, the profile's hidden screens still hide, and floating windows go relative to the screens wherever they are. ft-floatd goes through the entries app by app. It claims windows of that app already open (oldest first, each claimed once), and moves each to its entry's place: onto its screen at its rect (or maximized), or floating at its pose. For the entries left over, it launches the app once (`ft-float launch`, the same path as Launch as Standalone) and waits up to 30 seconds for its first window. Each window that shows up goes to the next entry's place. Once the first window has been up for 3 seconds (time for an app that restores its own windows to show them), ft-floatd launches the app again for each entry still waiting, and waits up to 30 seconds more. New windows are matched to the launch by process (or a child of it), or by desktop file name (found the same way as at capture): single-instance and D-Bus-activated apps open their windows from a process that was already running.
- **Default at start.** The session script runs `ft-layout start --wait 90`. That opens the profile in `FT_PROFILE` or `default_profile` (screens, then the apps once ft-floatd is up), or runs `apply --wait` if there's none. Start in profile on the Layout & profiles page sets `default_profile` (`ft-layout default NAME|none`). Plasma's session restore is turned off in the session (`ksmserverrc`: `loginMode=emptySession`).
- **Launcher entries.** Each profile gets `~/.local/share/applications/frametop-profile-<name>.desktop` ("Frametop: Work"), written when it's saved and removed when it's deleted. They show in SteamVR's Launch a program list, the Application Launcher, and KRunner. Running one (`ft-layout open NAME`) switches to that profile if the desktop runs. Otherwise it starts the desktop with `FT_PROFILE` set (`systemd-run`, as `desktops.sh start` does), which overrides `default_profile` for that start. That needs SteamVR to be running.
- **The quick reset.** Meta+Shift+R, the reset button on a screen's bar, the Reset Screen Layout menu entry, and a button mapped to Reset desktop screen layout run `ft-layout reset`: with a profile in use (`active`, and the custom arrangement), that's `use` on it again, so everything goes back as the profile has it. Without one, it's `apply`.
- **The action.** `profile:NAME` in the input relay (it runs `ft-layout use NAME`) for key combinations, mouse buttons, and controller buttons, with or without pointer mode. Input Settings lists one "Open profile NAME" action per profile.
- **Display Settings.** On the Layout & profiles page, the arrangement list has the profiles, which can be renamed and deleted. Open profile and Save as profile… are the page's actions. A profile's apps are listed with where each goes and a button to leave one out, plus which screens it hides. Start in profile picks the one the desktop starts with. The Visibility tab's Screens shown switches hide screens one at a time.
