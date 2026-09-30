# Gaze first: plan

Gaze first makes the eyes the pointer for everything flat in VR, and the controllers its buttons. With gaze on and no game running, the gaze drives Frametop's 3D pointer (the `ft_pointer` driver) everywhere the mouse can go, SteamVR's dashboard included. Either controller's trigger clicks where you look, and the controllers stop showing lasers. The work happens on branch `gaze-first` (worktree `~/frametop/.worktrees/gaze-first`, from `experimental`) and is merged into `experimental` after it's been tested in the headset.

The decisions below were made with the user on 2026-09-30. Nothing is built yet: the first step is a headset session with four tests (see "Tests before building").

## Decisions

Input, with gaze on and outside games:
- The gaze drives `ft_pointer`, so anything the mouse does today works with the gaze, on Frametop's screens, floating windows, the keyboard, and SteamVR's and Steam's panels. The mouse keeps working as it does today in gaze mode.
- The gaze wakes the pointer by itself: the headset is worn and the tracker sees your eyes. No mouse movement is needed.
- Trigger, on either controller:
  - A tap clicks where you look.
  - Moving the controller within `POINTER_GAZE_HOLD` (0.5 s) of the press is precision: the pointer stops where you looked, and your hand's movement steers it, relative, like tap and drag on the Apple Vision Pro (the controller's position seen from the eye, not where it points). The release clicks there, and the correction goes to the gaze service as a lesson.
  - Holding still for 0.5 s makes a real press; then the hand's movement drags 1:1.
- Bumper, on either controller: the same, with the right button.
- Right thumbstick: scrolls at the pointer (vertical, and horizontal when pushed sideways).

Controllers:
- They keep their hand roles and their stock SteamVR bindings: the Steam button (tap for the dashboard, double tap for the room view, hold to recenter, the screenshot chord), gamepad mode (both grips), locomotion, room setup, and every per-hand feature.
- Only the trigger and the bumper are muted from SteamVR while gaze first is on, so they never click or take the laser.
- When something still moves the laser to a controller (a Steam button summoning the dashboard, a grip), the helper moves it straight back to our device.
- Later: mute the grips too, and maybe have turning gaze off switch the controllers to gamepad mode.

Turning gaze on and off:
- Gaze is opt-in and off by default. On or off is remembered across restarts (`POINTER_GAZE`), whatever turned it on or off.
- Any game gets the controllers: a VR game (a scene application) or a flatscreen Steam game. Gaze first is off during one.
- The toggle macro, both thumbstick clicks held 1 s by default, turns gaze on or off. In a game it turns gaze first on for that game only, until the game exits.
- You can record your own toggle macro on the Gaze page of Frametop Input Settings: a chord of buttons on one or both controllers, held together for 0.5 to 2 s. The Steam button can't be part of it. It needs 2 or more buttons, or 1 button held at least 1.5 s. Firing it cancels a gaze click in progress. "Reset to default" brings back the thumbsticks. The one-button "Gaze pointer on/off" mapping and key combinations keep working.
- Gaze can't turn on without a calibration for the tracker in use, or without a working tracker service. Turning it on then opens the calibrator, or points to "Repair eye tracker".

Eye tracker:
- Our own tracker (`gaze/tracker/`) is the default. Choosing SteamVR's tracker on the Gaze page turns ours off. The same calibration rule applies to SteamVR's.
- Its root service, `frametop-eyegrab`, is installed by Frametop's installer (a step that defaults to yes) and stays enabled. Turning our tracker off means no longer asking it for frames: it then holds none of SteamVR's buffers, and nothing needs root.
- The tracker waits for a calibration before tracking, and idles whenever gaze input is off (tracking costs roughly 7 to 36 % of a core).
- If the service is missing or broken, gaze can't turn on, and the Gaze page offers "Repair eye tracker". There's no silent switch to SteamVR's tracker.
- Updates: the gaze service compares the installed `ft-eyegrab` with the build, and when they differ the Gaze page offers "Update eye tracker". Both repair and update ask for the password each time, through polkit (`pkexec`). There's no passwordless sudoers or polkit rule: the build output is writable by the user, so such a rule would let any program running as the user get root.

Calibration, in one head-locked panel:
- Quick check: one centre dot. It's captured by a dwell (about 0.6 s of steady fixation; steadiness, not position, so it works however far off the tracker is), the trigger accepts early, and it closes itself after about 4 s if ignored. It opens when the headset is put on, when our tracker notices the headset slipping (at most once every 2 minutes), and from a mappable action. If the first 3 nudges after it are still more than 2 degrees off, it asks for 5 dots.
- Full calibration: the same panel, about 60 degrees across, running the probe's calibration (21 dots in dark, medium, and bright rounds). Frametop's screens hide while it runs. It opens when turning gaze on finds no calibration. Quitting it leaves gaze off; turning gaze on again reopens it. Resetting the calibration while gaze is on turns gaze off and opens it.
- The dots move with your head, so there's no "keep your head still", and the calibration doesn't depend on where the screens are.
- The gaze probe stays as the lab tool.

## What exists

- Gaze mode (`POINTER_GAZE`, `pointer/helper/ft-pointer.cpp`): the gaze aims the pointer; the mouse's held-back press, precision, hold to drag, and nudge lessons are the model for the trigger.
- Holds in the helper (`struct Hold`): pinches and grips steer by the hand's movement seen from the eye, in the room, from where the eye was when the gesture began (`PoseHistory`). The trigger's steering is the same with the controller's position instead of the hand's.
- `gaze_precision` and `gaze_drag` (relay actions, "precision|gazedrag <source> 1|0" to the helper) steer by the controller's aim. Controller holds switch to position steering.
- Controller buttons through the helper's global action sets (`pointer/helper/vrbuttons.h`), one action set per button, active only for mapped buttons and only outside games.
- ft-screens' `controllers always|outside_games|dashboard` and its `hide`/`show` switch.
- Our tracker's headset-moved detection (`gaze/tracker/eyes_model.py`: a glint slip change over 3 px held 1 s) and its reseat after the frames stop.
- `gaze/tracker/install.sh` already installs `ft-eyegrab` as a system service with sudo.

## What SteamVR does (found 2026-09-30)

- The Frame controller's compositor bindings (`/opt/steamvr/drivers/frame_controller/resources/input/vrcompositor_bindings_frame_controller.json`) put the laser on a controller in only two ways: a button that clicks or switches the laser (trigger and bumper click; trigger, bumper, and grip move the laser to that hand with `switchlaserhand`), or summoning the dashboard with its Steam button. Everything else there doesn't touch the laser.
- The gamepad and laser modes are the `/actions/dualanalog` action set (`ModeSwitch1` and `ModeSwitch2` on the grips), with `dashboard.modalGamepadAndLaser`. Those actions are application-scoped: the mode lives in Steam's UI, and we can't set it from outside.
- The laser doesn't need a hand. The headset's own binding (`/opt/steamvr/drivers/frame_hmd/resources/input/vrcompositor_bindings_frame_hmd.json`) runs the laser from `/user/head/pose/raw`. Our device has only tried the right, left, and stylus roles; the stylus attempt got no role at all.
- A Frame controller in the hand takes its hand's role back through its touch sensors, so a device that needs a hand role loses it whenever both controllers are held. That's why the laser has to live somewhere else.
- vrserver's web socket (`/input/getstate.json` and `request_input_state_updates`, as frame-voice uses) lists each controller's `/input/trigger/click`, `/input/bumper/click`, `/input/grip/click`, `/input/thumbstick/click`, `/input/thumbstick/x` and `y`, `/input/system/click`, and each device's `side`. The headset has `/proximity`, but it flickers off for 0.3 to 0.5 s at a time while worn, so the quick check follows SteamVR's activity level (as the helper already does) rather than the raw sensor. Reading the socket takes nothing from anyone. A controller's path is `/devices/cv/<serial>` instead of `/user/hand/<side>` while our device holds that hand.

## Test results (2026-09-30)

Run with the headset on its stand and the controllers on (`pointer/probe/lasertest`, `input/vrws.py`):

1. **Laser on the treadmill role: works.** Our device held the dashboard laser with no hand role. SteamVR gives a device the `/user/treadmill` path only if it hints treadmill when it's added, so the driver hints a role that's no hand from `Activate`; a hint changed on connecting kept it at `/devices/ft_pointer/ft_pointer_0`. `GetControllerRoleForTrackedDeviceIndex` reports no role for it, so the helper's "no hand role" release skips treadmill.
2. **Muting through the helper's action sets: doesn't work.** SteamVR reported those actions inactive (`vrstatus`: `"active": []`) while its laser mouse had input focus, as frame-voice found on 2026-09-26, and a trigger pull moved the laser to its controller until the release. **Muting through the compositor binding works:** `pointer/bindings/vrcompositor_frame_controller_gazefirst.json` is the stock binding without its trigger and bumper laser entries, selected with `POST /input/selectconfig.action` on vrserver's port 27062 (a JSON body `{"app_key": "openvr.component.vrcompositor", "controller_type": "frame_controller", "url": "file:///..."}`; a form-encoded one gets "Parse failed"). `GET /input/getactions.json?app_key=openvr.component.vrcompositor` shows the choice (`current_binding_url`). With it, the triggers showed no laser. Selecting the stock file puts it back.
3. **Snap back: works** in 11 to 15 ms after a grip or a Steam button summon. A trigger held the laser until its release (0.4 to 1 s), which the muting removes. Our device also takes the laser from "none".
4. **Web socket: works.** Every click, the thumbstick axes, and both thumbsticks clicked together arrive. The headset's `/proximity` flickers off for 0.3 to 0.5 s at a time while worn, so the quick check goes by SteamVR's activity level instead.

Also found: the bumpers, the thumbsticks' movement, and their clicks switch Steam's dashboard into its controller (gamepad) mode, and the laser owner goes to "none"; both grips go back to laser mode. Steam's own Frame controller binding (`steam_vrgamepad_bindings_frame_controller.json`, app `steam.client`) has only haptics, so that input reaches Steam's UI some other way (most likely Steam Input's virtual gamepad), and a SteamVR binding can't mute it.

Decided after the tests: gaze replaces the laser pointer's controls, never the controller mode's. Controller mode takes precedence while it's on, and leaving it gives the laser back to the gaze. Right click is one grip held with a trigger (the bumpers belong to controller mode).

## Tests before building

One headset session, about 30 minutes, with the user wearing the headset. Installing the test driver needs a SteamVR restart, which closes everything in VR, so it's done at the start of the session and only when the user says so.

1. **Laser on the treadmill role.** The driver takes a `role treadmill` command, and its compositor bindings repeat the right hand's under `/user/treadmill`. Pass: after its `switchlaserhand` (`/input/a`), `GetPrimaryDashboardDevice()` is our device, and the pointer clicks Frametop's screens and the dashboard while both controllers are held. Fail: the fallback (below).
2. **Muting.** The helper activates its trigger and bumper action sets on both sides. Pass: a real trigger or bumper, pointed at a panel, neither clicks nor moves the laser, with the dashboard open and closed, and the helper sees the press. Fail: we need our own compositor binding for the Frame controller, switched when gaze first turns on and off.
3. **Snap back.** A Steam button tap on either controller, and a grip. Pass: the helper sees the laser move to a controller and moves it back within about 100 ms, with no stray click.
4. **Web socket.** Update rates for the thumbstick axes and clicks while held, and `/proximity` at don and doff.

Fallback if test 1 fails: gaze input goes straight into ft-screens for Frametop's own panels (the helper already knows which panel it hits and where), with both controllers held. SteamVR's and Steam's panels then take the gaze only while one hand is empty.

## Design by component

### Driver (`pointer/driver`)

- `role treadmill` joins `right`, `left`, and `stylus`. The hint stays OptOut while disconnected, as now.
- `ft_pointer_vrcompositor.json` and `ft_pointer_steam.json` get the same bindings under `/user/treadmill`. They're additive, so nothing changes while the device is a hand.

### Pointer helper (`pointer/helper/ft-pointer.cpp`)

- Gaze first = gaze mode on, the gaze service ready, no game, or a game with the macro's override.
- Waking: in gaze first, fresh gaze with the headset worn wakes the pointer (connect, treadmill role, `switchlaserhand`), with no mouse counts. The "no hand role" release doesn't apply to the treadmill role.
- The laser: while awake in gaze first, if the dashboard's primary device becomes a controller, press `/input/a` on ours again, at most every 100 ms. Last used wins stays off in gaze mode, as now.
- Muting: the trigger and bumper action sets on both sides are active whenever gaze first is on, whatever the relay's mappings say. Their presses go to the built-in state machine, not to the relay.
- Trigger and bumper: a hold with a new source, a controller's position. It starts as a held-back press at the gaze; moving past `POINTER_TRIGGER_DEADZONE` (about 1 degree, seen from the eye; the pull jolts the controller) within `POINTER_GAZE_HOLD` is precision at `POINTER_TRIGGER_GAIN` (0.5); still until `POINTER_GAZE_HOLD` is a real press, and dragging at `POINTER_GAZE_DRAG_GAIN` (1). Release: click, lesson (as with the mouse, up to `POINTER_GAZE_NUDGE_MAX`), or release the press. The existing controller holds (`gaze_precision`, `gaze_drag`) move to position steering too.
- Scroll: "scroll <dx> <dy>" from the relay goes to the driver, as the mouse's wheel does.
- Calibration: while ft-gazed says its panel is up ("calpanel 1|0"), the pointer hides, and a trigger press goes to ft-gazed as "calaccept" instead of clicking.
- The macro's "cancel": drops a held-back press without clicking.
- Headset worn or not goes to ft-gazed ("headset 1|0"), for the quick check.

### Input relay (`input/input-relay.py`)

- A web socket reader for vrserver, in the standard library (the host's Python has no `websockets` module, and the relay needs nothing but Python). It follows the controllers by `side`, since their paths change with the roles.
- The toggle macro: `"gaze_macro": {"buttons": ["left/thumbstick", "right/thumbstick"], "hold": 1.0}` in the rules file. Outside games it writes `POINTER_GAZE` and tells the helper; in a game it toggles the override for that game. It sends the helper "cancel" when it fires.
- Recording: Input Settings asks for "macro record" and gets back the chord and how long it was held, after the checks in "Decisions".
- Scroll: the right thumbstick's axes, while gaze first is on, as "scroll" lines to the helper.
- Games: a Steam game running (a `reaper` process with `SteamLaunch AppId=`, checked every 2 s; to confirm with a flatscreen game) goes to the helper, which already knows scene applications.
- `gaze_toggle` writes `POINTER_GAZE` instead of lasting until a restart.

### Gaze service (`gaze/ft-gazed`)

- `GAZE_TRACKER` defaults to `own`.
- Readiness goes to the helper ("gazeready 1|0"): the tracker's service works (for ours: `frametop-eyegrab` active, and its binary the same as the build) and the tracker in use has a calibration. When gaze is on but not ready, it opens the calibrator, or asks for the repair.
- Our tracker runs only while gaze is on and ready, while a calibration runs, or for the probe's lease.
- The quick check opens on "headset 1" from the helper, on our tracker's jump (at most once every 2 minutes), and on "quickcal" (the mappable action). For our tracker, the dot is a click on `@ft_eyes`, like the probe's one-dot check; for SteamVR's, it's a lesson.
- The full calibration's logic (the dots, rounds, sample rejection, and fits) moves out of the probe into `gazecal.py`, so the probe and the service share it. Quitting writes `POINTER_GAZE=0`.
- Hiding Frametop's screens during a full calibration goes through ft-screens' `hide` and `show`, keeping the user's own switch as it was.

### Calibration panel (`gaze/panel/ft-gazepanel`, new)

A small C++ OpenVR overlay program in the dev container: a head-locked overlay (placed relative to the headset) of a fixed angular size, drawn on the CPU and uploaded with `SetOverlayRaw`, like ft-screens' keyboard, with labels from stb_truetype. ft-gazed drives it over `@ft_gazepanel` (show quick or full, dot at yaw and pitch with its state, background brightness, a line of text, hide). It takes no input: the trigger comes through the helper, and the dwell is ft-gazed's. It's a separate program because the helper is already 2,000 lines, and the panel has nothing to do with pointing.

### Frametop Input Settings, Gaze page

- The gaze switch, blocked with the reason while not ready ("Calibrate first", "Repair eye tracker").
- Eye tracker: ours (default) or SteamVR's.
- Status: the service, the calibration, and when it was made. Buttons for Calibrate, Quick check, and Repair or Update eye tracker (`pkexec gaze/tracker/install.sh`, which then runs without sudo inside).
- The toggle macro: what it is, Record (a 3 s countdown, hold the chord, confirm), and Reset to default.

### Installer (`install.sh`)

- A step for our eye tracker ("It needs your password (sudo)"), defaulting to yes, and the gaze service, which needs no root. Gaze itself stays off.

## Order of work

1. The tests above, then this plan updated with the results.
2. Driver and helper: the treadmill role, muting, snap back, waking by gaze, and the trigger and bumper holds. Gaze still turns on the old way.
3. Relay: the web socket reader, scroll, the macro, games and the override, `POINTER_GAZE` remembered.
4. Gaze service: readiness, idling, the default tracker, the service check, and the quick check's triggers.
5. The calibration panel and calibrator: the quick check, then the full calibration and the move to 5 dots.
6. Input Settings, the installer, and the docs (`README.md`, `docs/design.md`, `docs/reference.md`, `gaze/README.md`).

Each step is tested in the headset before it's merged into `experimental`.

## Risks

- SteamVR may refuse the laser on a treadmill device, or a SteamVR update may change the Frame's compositor bindings.
- The muting relies on "Enable global input from overlays" (`steamvr/globalActionSetPriority`), which SteamVR calls experimental.
- `pkexec` runs a script the user can write. That's acceptable only because every run asks for the password.
- Our tracker's CPU cost while gaze is on, with SteamVR and a busy desktop; it idles otherwise.
- Head-locked panels can be uncomfortable; the panel stays small and short-lived, except for the full calibration.
