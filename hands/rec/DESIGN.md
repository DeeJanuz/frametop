# Hand recorder: design (Phase 1 of the hands plan)

The hand recorder guides a person through recording their hands with the headset's cameras. It saves the recordings as files, lets them review and delete anything, then exports a package to contribute to the open hand dataset. Recordings never leave the headset unless the person uploads them.

The plan this belongs to is `~/Desktop/Projects/frame-hands/notes/hands-plan.md` (on the maintainer's Frame). In short, the dataset trains a small hand model for Frametop's hand cutouts.

## Parts

| Part | What it does |
|---|---|
| `hands/rec/panel/ft-handpanel.cpp` (C++, dev container) | The headset panel: a SteamVR overlay fixed to the head that shows the prompts. It also places the "touch the dot" target in the room and logs head and controller poses. Driven over `@ft_handpanel`. |
| `hands/rec/session.py` (Python, no Qt) | The session runner. It reads `script.json`, starts and stops the recordings, and drives the panel. It reads the live hands file for feedback and writes each take's files. It also runs from the command line (`--dry-run`) for testing. |
| `hands/rec/script.json` | The guided script: sections, prompts, timings. |
| `hands/rec/poses/` | The pose pictures, `poses.json` and its PNGs (below). |
| `hands/rec/takes.py` (Python, no Qt) | Reads sessions and takes from disk: frame sets for review, deleted ranges, export (compress, strip, manifest, checksums). |
| `hands/rec/ft_handrec.py` + `main.qml` (PySide6 + Kirigami, dev container) | The desktop window: consent, the before-you-start checklist, the session controls, review, export and upload instructions. |
| `hands/rec/ft-handrec` | The host launcher, like `input-settings/ft-input-settings`. |
| `hands/rec/build.sh` | Builds `ft-handpanel` into `hands/rec/build/`, like `gaze/build.sh`. |
| `hands/rec/CONSENT.md`, `hands/rec/UPLOAD.md` | The texts the window shows. |
| `hands/rec/install.sh` | Installs the recorder on a Frame with Frametop: the dev container's packages, hands/build.sh, the panel, ft-camd's capabilities, and the menu entry (`uninstall` removes the entry). |
| ft-hands `--record-hz N` (done) | Records at most N frame sets a second. The recorder uses 10. |
| `hands/camcheck.py` (shared, standard library) | Are all four mono cameras running? The recorder runs it before a session and when a step sees no hands (below; `hands/README.md`, "Camera check"). |

Every part runs in the dev container, as ft-hands and Input Settings do. The host Python isn't used: it lacks PySide6 and zstd. `setup/dev-container.sh` gains `zstd`.

### The standalone Hand Recorder

[frametop-hand-recorder](https://github.com/Frametop/frametop-hand-recorder) is the recorder for people without Frametop. It pins this repo as a submodule and ships the parts above with its own window, a Qt Quick Controls `main.qml` sized for SteamVR's dashboard. Its release builds the binaries for the SteamOS host: ft-camd and ft-hands statically (`make LDFLAGS=-static`), and ft-handpanel against SteamVR's `libopenvr_api`. Everything runs on the host, the Python from a venv (`ft_handrec.py --qml ITS_QML --style Basic`).

Its tarball keeps this tree's layout (`hands/build/`, `hands/rec/`, `hands/models/`), so the paths here don't change. A `standalone.json` at the top marks it (`takes.standalone()`):
- `session.py` starts ft-hands directly, not through distrobox.
- session.json's `tool` and the manifest's are `ft-handrec <describe> (frametop-hand-recorder <version>)`.
- The repair hints say to run its install command again.

The recordings go to the same `~/.local/share/frametop/hands/contrib`, so either recorder shows both's sessions.

## Processes during a session

- **ft-camd** publishes the camera ring. If it isn't running, the session starts it as the transient user unit `frametop-handrec-camd.service`, the way `hands/ft-cutouts` starts `frametop-cutouts-camd.service` (needs `hands/build/ft-camd` with capabilities: `hands/run.sh caps`). An ft-camd already running from ft-cutouts or ft-handsctl is used as it is.
- **A tracking ft-hands** gives feedback through the hands file: which hands are seen, the palm's distance, the index tip. If none is running, the session starts `ft-hands --no-gestures --status 0` (unit `frametop-handrec-hands.service`). An ft-hands already running is used as it is.
- **A recording ft-hands** runs once per recording part: `ft-hands --record-only --record DIR --record-for SECONDS --record-hz 10 --status 0 --sides auto|0|1` (below, "Side cameras"). It runs as a plain child process of the session, ended with SIGTERM when the part ends. SIGTERM ends ft-hands' loop, and `Recorder` writes out its queue when it's destroyed. In step mode (below) a part is one step's countdown and hold, so a take has one part per step (about 40 in the hand poses); in auto mode a take is one part, plus one more after each pause. `--record-for` is only a safety net.
  - Why a process per part rather than one kept alive and paused: measured in the dev container with `ft-ringplay`'s ring (2026-10-02), `ft-hands --record-only` writes its first set 16-27 ms after it starts and ends 4-6 ms after SIGTERM, so a new part costs nothing the 3 s countdown doesn't cover. Every reader already takes parts in order (`takes.py`, `validate.py` through the export's single stream, the labeller's `fhl_io.py`, numbering `sets-10.bin` after `sets-9.bin`), ft-hands needs no new control, and nothing is written while a step waits. Before the hold starts the session checks that the part has written a set (`Recorder.has_data`, up to 3 s more), so the hold is recorded from its first frame.
- **Side cameras.** ft-camd can name the two side cameras the wrong way round (hands/README.md, "Which camera is which"). The tracking ft-hands decides from the hands within about 2 s of them being in view (`HANDS_SWAP_SIDES=auto`, hands/track/sides.h) and publishes that in `/run/user/UID/frametop-hands/sides.json`. With `HANDS_SWAP_SIDES` forced to `0` or `1`, the hands still decide what's published once they disagree (state `"forced, disagrees"`; `read_live` also corrects an ft-hands built before that). The session reads it (`sides.py`, `read_live`) and stores it in session.json `"sides"`. Each later part is recorded named right (`--sides 1` or `0`). Parts recorded before the decision use ft-camd's names (`--sides auto`; a record-only ft-hands can't tell), and readers rename them (`sides.py`). Each part's `names_swapped` goes into take.json `"parts"`. Without a tracking ft-hands nothing decides: `"swapped": null`, the names stay as recorded, and the maintainer's check (`hub_review check`, check_sides on a few sets per take) tells. `takes.py sides SESSION --set swapped|named` records a decision by hand.
- **ft-handpanel** runs as a child process with `--watch-stdin`. It shows the panel and logs poses during each take.
- **The headset button's reader** is a thread of the session (`ButtonReader`, below), not a process.

Test hooks:
- `--ring PATH` goes to both ft-hands (`ft-ringplay` publishes a recording there, so the whole flow runs without the headset).
- `--no-start` uses only what's already running.
- `--dry-run` runs no processes and only prints the panel commands, with timing sped up by `--speed X`.
- `--next-after S` presses Next by itself after S seconds of waiting (real time), so a step-mode session runs unattended. Lines on stdin steer it too: `n` or an empty line is Next, `p` pause or resume, `r` redo, `s` skip the section, `q` stop.
- `--no-headset-button` leaves the headset's button alone (`ft-handrec --no-headset-button` too). `--button-device PATH` reads it from PATH instead, an event device or a FIFO of `input_event` structs, also in a dry run: a simulated button for tests.
- `--auto` runs the timed flow; `--poses DIR` takes the pose pictures from DIR; `--plan` prints the sections, their steps and length.
- `--ignore-cameras` (`ft-handrec --ignore-cameras` too) starts even when the camera check fails. A dry run and `--ring` skip the check by themselves.

## Files

```
~/.local/share/frametop/hands/contrib/
  profile.json                  consent and contributor id (below)
  sessions/<YYYYMMDD-HHMMSS>/   (a second session started in the same second gets -2, and so on)
    session.json                the session: checklist answers, lighting, versions, mode, takes
    calibration.json            /persist/xrservice.json with identifying fields removed (below)
    device.json                 the rig's pose in the CAD frame from /persist/device_config.json (below)
    takes/<NN>-<section>/
      sets.bin                  ft-hands' recording (FHSET01, hands/track/record.h), 10 sets/s: the first part
      sets-2.bin, sets-3.bin, ...  the next parts (step mode: one per step; any mode: after a pause)
      prompts.jsonl             what the person was asked to do, when (below)
      poses.jsonl               head and controller poses from ft-handpanel (below)
      take.json                 {"section", "title", "started_ns", "ended_ns", "status": "complete"|"stopped"|"skipped",
                                 "deleted": [[from_ns, to_ns], ...], "notes": "",
                                 "parts": {"sets.bin": {"names_swapped": false}, "sets-2.bin": {...}},
                                 "clock": [[mono_ns, raw_minus_mono_ns], ...]}
  exports/<session>/            what export writes (below)
```

All `_ns` times are CLOCK_MONOTONIC nanoseconds, the clock of `dqbuf_ns` in sets.bin and of `capture_ns` in the hands file. sets.bin's `capture_ns` is the camera clock (CLOCK_MONOTONIC_RAW). The two drift apart with NTP's corrections: on 2026-10-03 RAW ran 0.80 s ahead, gaining about 10 ppm. take.json `"clock"` samples the difference (RAW minus MONOTONIC, as ft-hands' `raw_minus_mono_ns()`) when each part starts and stops, so a set's exposure time on the poses' clock is `capture_ns - raw_minus_mono_ns`, interpolated between samples. `dqbuf_ns` is on the right clock already, but a few ms after the exposure. Takes recorded before 2026-10-03 have no `"clock"`.

### profile.json

```json
{"schema": 1, "contributor": "<random uuid4>", "consent": {"version": "2026-10-02", "accepted": "<ISO time>", "adult": true},
 "optional": {"handedness": "right|left|both|", "notes": ""}}
```

No name, email, or account. The contributor id is random, so several sessions from one person can be held out together in evaluation. Withdrawal also goes by that id.

### session.json

```json
{"schema": 1, "tool": "ft-handrec <git describe>", "started": "<ISO>", "contributor": "<uuid>",
 "lighting": {"chosen": "dim|room|daylight|indoor", "source": "measured|picked", "measured": "indoor|daylight|",
              "ambient_ir": 0.0, "ring": {"<cam>": {"mean": 0.0, "dark_mean": 0.0}}},
 "checklist": {"objects": ["pencil", "phone", "cup", "keyboard", "mouse", "gamepad", "small"], "own_objects": ["..."],
               "controllers": "straps|none", "sleeves": "short|long|", "rings": false, "watch": false, "notes": ""},
 "device": {"steamos": "<VERSION_ID from /etc/os-release>", "steamvr": "<version if known>", "cameras": [{"name", "width", "height"}]},
 "mode": "step|auto", "quick": false, "shuffle": {"seed": 123, "sweeps": {"pose-sweeps": [{"id", "hands", "cues"}]}},
 "takes": ["01-hand-size", "..."],
 "camera": {"status": "ok|unknown|degraded", "reason": "..."}, "stop_reason": "...",
 "sides": {"swapped": true|false|null, "decided_by": "auto|config|option|manual", "state": "decided|confirmed|...",
           "evidence": {"as_named": 0, "swapped": 10, "seconds": 1.8, "miss_mm": [-1, 4.2], "probes": 30, "found": 10},
           "decided_at": "<ISO>", "decided_ns": 0}}
```

`sides`: whether ft-camd's side camera names were backwards during the session (`swapped`), as the live tracker decided it (above, "Side cameras"); `null` while nobody knows. A part's names are right when its take.json `names_swapped` equals `swapped`. Parts without a `parts` entry were recorded with ft-camd's names. Sessions from before this have no `sides`.

`camera` is the camera check's verdict at the start (no paths or log lines: those go to `session.log`). `stop_reason` is there when a session was stopped at the no-hands screen, with the check's result.

A session started before step mode existed has no `mode`: it ran as `auto`. `quick` and `shuffle` (below, "Sweeps" and "Quick round") are missing from sessions before the sweeps.

### calibration.json

This is a copy of `/persist/xrservice.json` (`/run/host/persist/` in the container). Keep the cameras' intrinsics and extrinsics, and drop anything that identifies the unit: keys containing `serial`, `sn`, `uuid`, `mac` or `id`, or values that look like serial numbers. List what was removed in `session.json` (`"calibration_removed": [...]`), so a reviewer can check it.

### device.json

The head frame needs the rig's pose in the CAD frame, from `/persist/device_config.json`. Only two of its keys are kept, `cv.cad_from_cal` (Cam0 in the CAD frame) and `head` (the head in CAD), in the shape the labeller in frame-hands `train/label` reads, as its `cut.py` writes it:

```json
{"cv": {"cad_from_cal": {"method": "FrontAndUpperCamPositions", "plus_x": [x, y, z], "plus_z": [x, y, z], "position": [x, y, z]}},
 "head": {"plus_x": [x, y, z], "plus_z": [x, y, z], "position": [x, y, z]}}
```

The rest of that file identifies the unit (serial number, display EDID) and is never copied. The two kept keys go through `strip_calibration` as well, and anything it removes is listed in `calibration_removed` as `device.json:<path>`. Sessions recorded before device.json existed have none: they still validate, with a warning, and the labeller falls back to another unit's pose.

### prompts.jsonl

One JSON object per line:

```json
{"t": 123, "event": "take", "section": "static-poses", "take": "03-static-poses"}
{"t": 123, "event": "prompt", "id": "static-poses/fist/left/near", "text": "...", "hands": "left|right|both|none",
 "pose": "fist", "distance": "near|mid|far|", "position": "centre|left|right|up|down|", "object": "", "controller": false}
{"t": 123, "event": "target", "id": "touch/3", "head": [x, y, z], "room": [x, y, z], "state": "show|hold|done|timeout"}
{"t": 123, "event": "bar", "target": 0.0}
{"t": 123, "event": "feedback", "left": true, "right": false, "palm_m": [0.0, 0.0]}
{"t": 123, "event": "pause"}
{"t": 123, "event": "resume"}
{"t": 123, "event": "ready", "id": "static-poses/fist/left/near", "seconds": 3}
{"t": 123, "event": "wait"}
{"t": 123, "event": "redo", "id": "static-poses/fist/left/near", "from": 123, "to": 123}
{"t": 123, "event": "nohands", "id": "hand-size/flat/both", "reads": 120, "published": 118}
{"t": 123, "event": "end", "status": "complete|stopped|skipped"}
```

`feedback` is written about twice a second while recording. It's the live tracker's view, kept for later checks; it's not a label.

A prompt holds from its `prompt` until the next `prompt`, `ready`, `wait` or `end`. A step in step mode reads:

```
ready      Next pressed: recording part N starts, the 3-2-1 countdown runs (recorded, no label)
prompt     the hold: its labels start here
(bar, target, feedback, pause/resume during the hold)
wait       the hold is over: no labels from here; part N stops
```

The touch-the-dot targets after the first follow straight on: no `ready` or `wait` between them. `redo` marks a try done again (R): `from` is that step's `ready` (or its `prompt` if it had none), `to` its end. Its prompt is skipped; the sets stay. In auto mode there's no `ready` or `wait`, and the intro is a recorded prompt `<section>/intro`. `nohands` marks the first hand-size step stopped because no hand was seen (below, "Camera check"); a `redo` over the same range follows it, so the try gets no labels. A sweep step (below, "Sweeps") has a `prompt` per cue, each with its `pose`, `"cue": true` and `"step"`, the step's id; its `ready`, `wait` and `redo` carry the step's id. Readers that knew only `prompt` and `end` keep working, but they'd give the countdown to the step before: `hub_review.py` (frame-hands `train/hub`) shows it as "(countdown)", redone prompts as "(redone)" and cues as "[pose] text"; `FORMAT.md` in the dataset repo has the rules.

### poses.jsonl

ft-handpanel writes one line per sample, 250 a second, from `GetDeviceToAbsoluteTrackingPose(TrackingUniverseStanding, 0)`:

```json
{"t": 123, "hmd": {"m": [12 floats, row-major 3x4], "r": 200, "ok": true},
 "left": {"m": [...], "r": 200, "ok": true}, "right": null}
```

`r` is `ETrackingResult` (200 = Running_OK, 201 = Running_OutOfRange, and so on). `left` and `right` are the devices holding those controller roles, or null. No device serials are logged.

## The panel (`ft-handpanel`)

- **Placement.** A SteamVR overlay fixed to the head, like `gaze/panel/ft-gazepanel.cpp`: key `frametop.handpanel`, sort order 250. It sits 1.2 m ahead, centred 12 degrees above straight ahead, so the hands stay clear below it. It's 36 degrees wide, 4:3, 1024x768 pixels, dim and see-through. It's drawn on the CPU with stb_truetype (and stb_image for the pose pictures, PNG only, the same pinned stb commit) into three shared DMA-BUFs SteamVR imports once, as ft-gazepanel does, and is drawn again only when something changes.
- **Layout.** The title and step on top (a red "Rec" by the step while recording), a rule. With a pose picture or a diagram, a 14-degree column on the left holds the picture (or the flipped copy and the picture side by side) and the where-to diagram under it; the text takes the right. The text column holds the instruction, the orange note, the big countdown ("3", "2", "1", then "Hold" or "Go") and the cyan action line ("Ready? Press Space or click Next"), centred together. At the bottom: the near/far bar, the hand chips, the time-left bar and the key hints.
- **Socket.** Abstract unix datagram `@ft_handpanel` (`--socket NAME`). A sender with an address gets `ok ...` or `error ...`.
- **Options:** `--watch-stdin` (quit when stdin closes), `--socket NAME`, `--distance M`, `--no-vr`. `--no-vr` makes no SteamVR connection and prints each picture's text to stdout: for testing without a headset.

Commands (UTF-8; `|` starts a new line in text):

| Command | Effect |
|---|---|
| `show` / `hide` | The panel. A "show" makes it visible with its first picture. |
| `title <text>` | Big line at the top. |
| `step <text>` | Small line at the top right, e.g. `Section 3 of 11`. |
| `text <text>` | The instruction, large, wrapped to the panel's width, centred. |
| `note <text>` | An orange line under the instruction: a warning ("I can't see your left hand"). Empty clears it. |
| `countdown <0..1>` / `countdown off` | A thin bar along the bottom: the share of this prompt's time left. |
| `hands <left> <right>` | Two chips, "Left hand" and "Right hand", each `seen` (green), `lost` (orange) or `off` (hidden). |
| `bar <target 0..1> <current 0..1 or -1> [near label] [far label]` / `bar off` | The near/far bar for the push out and back: a horizontal track with a target marker and the hand's current position. |
| `paused on` / `paused off` | A "Paused" overlay over the picture. |
| `image <path> [mirror\|both]` / `image off` | The pose picture, a PNG (below), in the left column. `mirror` flips it (a left hand); `both` draws a flipped copy on its left. A file that can't be read: `error ...` and no picture. |
| `where <position\|-> <distance\|->` / `where off` | The where-to diagram under the picture: a 3x3 front view with the asked cell lit (`centre`, `left`, `right`, `up`, `down`, and the push sections' `chest`, `desk`, `eye`), and a side view of the head and three marks for `near` ("Close"), `mid` ("Halfway out") and `far` ("Arm out"). `-` leaves that half out. |
| `action <text>` | The cyan line under the instruction (empty clears it). |
| `big <text>` | Large cyan text under the instruction: the countdown (empty clears it). |
| `keys <text>` | The faint key hints along the bottom. |
| `rec on` / `rec off` | The red "Rec" by the step line. |
| `strip <cue> <path>\|<mode>\|<label>;...` / `strip off` | A sweep's row of pose pictures (path `-`: none; mode `-` or `mirror`), each with its label under it; the one at index `cue` (from 0, -1 for none) framed in cyan and bright, the others dim. It sits along the bottom of the space left, the where-to diagram at its right end if there is one, and the text above it; it takes the left column's place. Each picture is loaded once. A file that can't be read: `error can't read ...`, and that item shows an empty frame. |
| `target <x> <y> <z> [show\|hold <0..1>\|done]` / `target off` | The touch target: a small sphere-like dot about 2 cm across, in its own overlay (`frametop.handpanel.target`). The point is in the head frame (metres, +x right, +y up, -z forward). The first `target` with a new point places it in the room with the current HMD pose, and it stays there. Later commands with the same point change only the state. `hold` draws a filling ring, `done` turns it green. Reply: `ok <room x> <room y> <room z>`. |
| `poses start <path>` / `poses stop` | Log poses to the path (appending, `poses.jsonl` format above) from a thread at 250 Hz, until stopped. |
| `devices` | Reply: `ok hmd <r> left <r or -> right <r or ->`, the current `ETrackingResult` values (`-` for no device in that role). |
| `head` | Reply: `ok <12 floats>`, the current HMD pose (standing universe). |
| `ping` | `ok shown` or `ok hidden`. |

It quits on SteamVR's quit event, as ft-gazepanel does. `--no-vr --dump DIR` writes each picture to `DIR/panel.pam`, to check the layout without a headset.

## The pose pictures (`poses/`)

`poses/poses.json` maps the script's `pose` ids to pictures: `{"<pose>": {"file": "<name>.png", "two_hands": false, "caption": "..."}}`. Each PNG is RGBA, square (512x512), drawn as the wearer sees it: a right hand, unless `two_hands` (then it shows both). How a prompt shows it (`session.pose_view`):

| Prompt's `hands` | Picture |
|---|---|
| `right` (and `any`, `none`, empty) | as drawn |
| `left` | flipped left to right (`image ... mirror`) |
| `both`, not `two_hands` | a flipped copy on the left, the picture on the right (`image ... both`) |
| anything, `two_hands` | as drawn |

A pose with no entry, or whose file is missing, shows no picture: the text alone. The session reads `poses.json` when it starts. The window shows the same picture (QML `Image.mirror`), its caption, and the same diagram.

## The script (`script.json`)

```json
{"version": 1,
 "cue_names": {"thumbs-up": "Thumbs up", "...": "..."},
 "sections": [
   {"id": "hand-size", "title": "Hand size", "requires": [], "intro": "text shown before the first prompt: 4 s, or until Next", "go": "Hold",
    "quick": true, "prompts": [{"text": "...", "cues": ["flat", "flat-back", "spread"], "cue_s": 5, "hands": "both", "distance": "mid"}]},
   {"id": "pose-sweeps", "title": "Hand poses", "kind": "sweep", "quick": true, "cue_s": 4, "step_s": 20,
    "groups": [["open", "fist", "point", "pinch"], ["ok", "thumbs-up", "spread", "claw"], ["count-1", "...", "count-5"]],
    "sweeps": [{"hands": "both", "group": "next", "text": "..."}, {"hands": "left", "group": "any", "quick": false, "text": "..."}]},
   {"id": "objects", "title": "Things you hold", "requires": ["objects"], "for_each": "object",
    "prompts": [{"text": "Pick up the {object} and use it the way you normally would.", "seconds": 15, "hands": "both", "object": "{object}"}]},
   {"id": "touch", "title": "Touch the dot", "kind": "targets", "hold_s": 1.0, "timeout_s": 8,
    "targets": [[0.0, -0.15, -0.40], ...], "text": "Touch the dot with your index fingertip and hold still."},
   {"id": "controller-push", "title": "Depth with controllers", "requires": ["controllers"], "kind": "bar",
    "intro": "...", "heights": ["chest", "desk", "eye", "left", "right"], "reps": 5, "period_s": 6, "near_m": 0.2, "far_m": 0.6}
 ]}
```

- `requires`: `objects` (at least one object ticked), `controllers` (straps ticked). A section whose requirements aren't met is skipped and logged.
- `go`: the word the countdown ends on, "Go" unless set ("Hold" for the still poses).
- `quick`: the section is in a quick round; a step with `"quick": false` isn't (below).
- `kind`:
  - `prompts` (the default): each prompt shows for its `seconds` with a countdown. A prompt with `cues` (and `cue_s`) is a sweep step with those cues in that order, `seconds` = cues x cue_s (hand size; the mouse and switch step).
  - `sweep`: steps built from `sweeps` (below).
  - `targets`: each target shows until the live index tip is within 3 cm of it for `hold_s`, or `timeout_s` passes.
  - `bar`: the target marker sweeps near to far and back, `reps` times per height, at `period_s` per sweep. The current marker follows the live palm distance.
- Each section is one take. Prompts within it are marked in `prompts.jsonl`.
- `cue_names`: the short labels under the strip's pictures (otherwise the pose id).

### Sweeps

The second in-headset session (sessions/20261002-202734) took about 16 min and was "super long and kind of annoying": the 36 held poses alone took 7.9 min, 3.2 of it reading, and the person skipped the wrist turns and gave up on the bare push after 3 steps. The labels come from the auto-labeller (teacher model and triangulation, frame-hands `train/label`), not from the prompts, so what the recordings need is variety (shapes, distances, angles), not clean holds. So the poses are swept:

- A sweep step shows a strip of 2-5 pose pictures and asks for slow, continuous movement (near and far, all around) while the hands change shape with the lit picture. The light moves on every `cue_s` (4 s), cycling, over `step_s` (20 s): 5 cues for a group of 4 or 5. Each cue is a `prompt` event with that `pose`, `"cue": true` and `"step"` (the step's id); `distance` and `position` are "" (varied). So the timeline tags each pose roughly, and the countdown, `wait` and `redo` work per step as before. The time-left bar covers the whole step.
- `groups`: lists of poses. A step takes `"group": "next"` (the groups in turn) or `"any"` (one drawn at random, a different one for each "any" while there are groups left), or names its own `cues`. `"shuffle": false` (gestures) keeps the script's order; `"cycle": false` runs the cues once; `"fixed": true` keeps one step's order.
- The pose sweeps: 3 two-hand sweeps, one per group ([open, fist, point, pinch], [ok, thumbs-up, spread, claw], [count-1 ... count-5]); then a left-hand and a right-hand sweep (the other hand in the lap) on groups drawn from those three, which also turn the wrist as they go, so the wrist angles come for free (the old wrist-turns section is gone).
- **The shuffle, per session.** The groups' order, the "any" draws and the cues' order in each step are shuffled with a seed from the session's id (`session_seed`: the first 8 hex digits of its SHA-256), separately per section (`random.Random("<seed>/<section>")`). The sections' order isn't shuffled (the controller sections need their before and after). session.json records `"shuffle": {"seed", "sweeps": {section: [{"id", "hands", "cues"}]}}`, and `build_plan(script, checklist, seed=...)` gives the same again. `--seed N` overrides it; `--plan` without one shows the script's order.
- The strip has one picture per pose, a single hand: both hands make the same shape, and five pairs wouldn't fit. A left hand's pictures are flipped.
- Gestures are sweeps too, in order and once: tap then drag; grab, cross, overlap; near the face then a screen's distance. Hand size is one step of three held shapes (flat, backs, spread; 5 s each), still the no-hands check's first step.

### Quick round

For more lighting rounds: "Quick round (about 3 min)" on the checklist page, `session.py --quick`. It has the sections marked `quick` (hand size, the pose sweeps without the one-hand ones, touch the dot, no hands), about 2 min recorded in 6 steps. session.json gets `"quick": true`. The checklist page suggests it (and picks it) once a full session has gone to the end (`Backend.hasFullSession`: a session.json with status `done`, not quick, not a dry run).

### Step mode (the default) and auto mode

The first in-headset session (2026-10-02) went too fast: each prompt advanced after 4-8 s, before there was time to read it and find the hand shape. So by default every step waits:

1. **Ready.** The panel shows the step: section title, "step N of M", the instruction, the pose picture, the where-to diagram, and the Next hint ("Ready? Press the button on the right side of the headset", or with a mouse also Space and Next: see Controls). The hand chips show which hands are seen, with no warnings yet. It waits as long as it takes, and nothing records.
2. **Countdown.** Next starts a new recording part and a "ready" event, and the panel counts 3, 2, 1 (big), recorded so the hold is captured from its first frame. In the push sections the bar sits at near meanwhile.
3. **Hold.** The `prompt` event, the section's word ("Hold" or "Go") and the time-left bar for the prompt's seconds (or the bar's sweeps, or the targets). Then a `wait` event and the part stops.

Steps that wait: each prompt, each bar height, and the first touch-the-dot target (the others follow straight on, as each waits for the touch anyway). Before a section, one screen shows the section's intro (with its `before` text, such as putting on the controllers) and waits for Next too; the welcome screen as well. The take starts with the section's first countdown, so a section skipped at its intro leaves no take. A pause in a hold works as before (the part stops; resume starts the next one).

Auto mode ("Advance by itself" on the checklist page, `session.py --auto`) is the old timed flow: the welcome, the between and before screens, the intro (recorded) and each prompt for its seconds, one recording per take. R still works there: it restarts the step running.

Steps are 20 s sweeps, 16-18 s gestures, 10-12 s desk and object steps, the touch targets (6) and the push heights (2, 3 reps of 6 s). The core session (no objects, no controllers) records about 5 min in 15 steps; with 5 s of reading a step that's about 6 min. Everything ticked (four objects, controllers): about 7.3 min recorded in 24 steps. A quick round: about 2 min in 6 steps. The window and `session.py --plan` give these (`plan_summary`); in step mode they leave the reading time out and say so.

The sections, in this order (see the plan):
1. hand size (one step: flat, backs, spread)
2. pose sweeps (above: 3 with both hands, one with each hand)
3. gestures (3 sweeps: pinch taps and drags; grabs, crossing and overlapping; near the face, then pointing at screen distance)
4. desk work (typing or pretend typing; the mouse, with switching to the keyboard when both are there)
5. objects (one prompt per ticked object, own objects included)
6. touch the dot (6 dots spread near and far, left and right, low)
7. controller depth, straps (push out and back at chest and eye level, 3 reps each; then the wrists and fingers with the controllers on). "Out and back" is straight away from the headset and back toward it: the first in-headset session took the left-right bar for sideways. The texts say so, the bar's ends read "At your chest" and "Arm out" (`near_label`, `far_label`, sent as `bar ... At your chest|Arm out`), and the picture is a side view.
8. bridge (one controller on, the bare fingertip on its thumbstick while that hand moves from close to arm's length and back, then swap; then a controller on the desk, touched from the front and above, then from the sides: 4 steps, the controller changes during the ready screens)
9. bare repeat of section 7's pushes, controllers off
10. no hands (10 s)

Before section 7: "Put on both controllers and tighten the straps". Before section 9: "Take the controllers off and put them out of view".

### Feedback while recording

- **Hands seen:** from the hands file. A hand counts as seen if its flags match the side and the file is fresh (publish within 0.3 s). Prompts with `hands` set show the `hands` chips. If an asked-for hand is lost for more than 1.5 s, the note says "I can't see your left hand: bring it into view".
- **Controller tracking:** in sections 8 and 9, `devices` is polled once a second. A result other than 200 for more than 1 s says "The left controller lost tracking: turn your palm slightly toward you". Each such stretch goes into `prompts.jsonl` as `feedback` with `"controller": {...}`.
- **Lighting, measured:** the checklist page measures the light when it opens, starting ft-camd (`frametop-handrec-camd.service`) if nothing runs it; the window stops it again on quit if it started it. The mean of every mono camera's `mean` and `dark_mean` from the ring (`hands/tools/ring.py` layout; struct only, no numpy) is compared with the person's earlier sessions. If it matches an earlier round's within 15%, the window says so before starting.
- **Lighting label:** "Measured by the cameras" is the default. `ambient_ir`, the mono cameras' mean `dark_mean` (the room's infrared), labels the round `daylight` from 6.0 and `indoor` below (`session.classify_lighting`). Lamps and LEDs give off hardly any infrared, so a dim room and a lit one read about the same (1.8 by one lamp, 2.2 in a lamp-lit room) and the cameras can't tell them apart; the person can pick dim, room or daylight instead (`source: "picked"`). The 6.0 threshold is a guess until a daylight round is measured.

### Camera check

On 2026-10-02 a whole session showed "I can't see your hands": after the headset slept, SteamVR had failed to load the colour module's FPGA image, which left the upper cameras and the IR light off (`hands/README.md`, "Camera check"). Two checks keep that from wasting a session:

- **Before the session.** The window runs `hands/camcheck.py` when the checklist page opens ("Tracking cameras:", with the evidence under Details and Check again), and again when Start is pressed; `session.py` runs it first thing, before it makes the session's folder or starts anything (`Session._preflight`). If it finds the cameras degraded, the session doesn't start. The window says: "The headset's upper cameras and IR light are off. SteamVR couldn't start the colour camera module (it happens sometimes after the headset sleeps). Restart SteamVR, or restart the headset if that doesn't fix it." (another `degraded:` reason gets a sentence naming it), and Start stays off. `unknown` (SteamVR not running, the cameras asleep) doesn't stop it: the session's own start fails clearly then. From the command line, `session.py` prints the check and exits with status 3.
- **Restart SteamVR.** The message has a Restart SteamVR button. After a confirmation it runs `systemctl --user restart --no-block steamvr.service` on the host (through `distrobox-host-exec`), then checks the cameras every 3 s, for up to 2 minutes, until a new XRService has opened its cameras. The confirmation says what really happens: every VR app closes, and so does the Frametop desktop with all its windows, this one included, and it doesn't come back by itself (`hands/README.md`, "What a SteamVR restart does to Frametop"). So the check after the restart mostly happens when the recorder is opened again; it re-runs when the checklist page opens. `--no-block` lets the restart finish after the window is gone.
- **No hands in the first step.** The first step of the hand-size section has both hands up, about 40 cm away. During its hold, the session counts the hands file's reads (about 20 a second). If the tracker published in at least half of at least 10 reads and never saw a hand, the step stops: a `wait` and the recording stops as usual, then a `nohands` event and a `redo` over the try. The session runs the camera check and shows "I can't see your hands" with its result (the camera text above when it's the VCINT failure, else "The camera check found nothing wrong"). The state is `nohands`, waiting: Next (the headset button, Space, Try again) or R starts the same step's countdown at once; Stop (Esc) ends the session with `stop_reason` set; S skips the section. One missed step costs a retry, not the session, and every hold of that step is logged ("hands check ...: a hand in N of M reads"). Without a tracker publishing the session can't tell, logs that, and goes on. Only that one step is checked: later steps have their notes ("I can't see your left hand") as before. Auto mode does the same; its recording pauses meanwhile.

### Controls

- The window has Start, a big Next (while a step waits; its hint is the panel's), Pause/Resume, Redo step, Skip section and Stop. While it has focus: Space is Next, P pauses or resumes, R redoes, S skips the section, Esc stops. The panel's bottom line and the window list them. The window also shows the step's picture, diagram, countdown and "Hold".
- **The headset's button.** The Frame has a click button on its right side, for use without controllers: `KEY_SELECT` (353) on the evdev device `gpio-keys`. It has three gestures (`ButtonGestures`), so a session can run with the window out of reach (the standalone Hand Recorder's window is in SteamVR's dashboard, closed while recording):
  - **A press.** While a step waits (the welcome, a section's intro, a step's ready screen, the no-hands question) it's Next; during a countdown or hold (and auto mode's timed screens) it pauses; while paused it resumes. It counts once 0.45 s have passed without a second press, so Next and pause come that much after the press.
  - **Two presses**, the second down within 0.45 s of the first one's release: redo, as R. It counts at the second press. Where R does nothing (nothing recorded yet, a section's intro), neither does this.
  - **A hold** of 1.5 s: stop, as Esc. It counts after 1.5 s, while the button is still down.
  - The session finds the device in `/proc/bus/input/devices` by name and its KEY bitmap (`event3` on the maintainer's Frame) and reads `input_event` structs with plain `struct` (no python-evdev), from a thread. Downs (value 1) and ups (value 0) count; autorepeat (value 2) doesn't. The gaps between events come from the kernel's timestamps, so a double press read in one go still counts as two. A release and a press closer than 30 ms are one press (gpio-keys debounces too).
  - It's read, never grabbed (`EVIOCGRAB`). Frametop's input relay (`input/input-relay.py`), ft-powerd, SteamVR and gamescope read the same device, and the relay remaps its volume keys (the experimental branch's `docs/hazards.md`). A grab would take the volume keys from the relay.
  - `steamos` is in the `input` group, so it needs no sudo. The dev container sees the host's `/dev/input` and `/proc/bus/input/devices`, and the group carries over (checked 2026-10-02), so it works from the window there and from `session.py` on the host. If the device is missing or can't be opened, the session logs it, keeps trying every 5 s, and the hints don't mention the button.
  - **Not known yet (needs the headset):** what SteamVR and gamescope do with the same press. They read it too, so it may also click whatever is under the head or gaze pointer in VR, or open something, and a hold or a double press may mean something else to them. Check on the first session; if it does, the fix is on their side or a different button, not a grab.
- **The Next hint follows what's there.** No mouse connected: "Ready? Press the button on the right side of the headset" (the window's Next can't be clicked, and Space needs the window focused). A mouse: "Ready? Press Space, click Next, or press the headset button". No button: "Ready? Press Space or click Next". The panel's bottom line leads with "Headset button: press for next or pause · press twice to redo · hold to stop", then the window's keys by letter. With the button, the pause note and the no-hands question say how to resume or stop with it too. A mouse is a device in `/proc/bus/input/devices` with EV_REL, REL_X and REL_Y that isn't made in software: uinput devices (Frametop's virtual mouse, frame-voice's keyboard) sit under `/devices/virtual/input` or on the virtual bus (6), and are left out; Bluetooth mice come through uhid, under `/devices/virtual/misc/uhid`, and count. It's looked at again at each step, so a mouse plugged in mid-session counts from the next step.
- **R (redo).** During a step's countdown or hold: that step starts again from its ready screen. At a step's ready screen: the step before it (in this section) goes again. Either way a `redo` event marks the range of the try being redone, so its labels are skipped; the sets stay, to delete in review if wanted.
- A pause stops the take's recording and starts it again on resume as the next part of the same take (`sets-2.bin`, and so on). takes.py reads all parts in order. A pause while a step waits only shows "Paused"; a Next pressed while paused doesn't count.

## Review and export (`takes.py`, the window)

- **Review.** Each session lists its takes: title, duration, status, a thumbnail (the first set's `slam_left`). A viewer shows one frame set (all cameras side by side, 8-bit grey) at a time with a slider. It can mark a range and delete it. Deleted ranges go into `take.json`, and export leaves them out; the files keep everything until export. A whole take or session can be deleted (files removed, after a confirmation).
- **Export.** It writes `exports/<session>/`:
  - `manifest.json`: profile fields except `optional.notes` unless kept, session.json (without its `uploads` records), takes, schema, tool version, the consent version.
  - `calibration.json` and `device.json`, when the session has them.
  - Per take: `prompts.jsonl`, `poses.jsonl`, `take.json`, and `sets.bin.zst` (sets in deleted ranges removed, then zstd -10 with 2 threads).
  - The side cameras are named right in every exported set when the session's `sides.swapped` is known: parts that need it get slam_left and slam_right (and their `_dk`) exchanged in the set headers as they're compressed. The exported take.json says `"parts": {"sets.bin": {"names_swapped": <swapped>}}`, and the manifest's take entry `"sides": {"names_swapped", "renamed_sets"}`. Unknown, the names stay as recorded.
  - `poses.jsonl` and `prompts.jsonl` without what's in the deleted ranges: no poses and no live-tracker `feedback` there (the prompt timeline stays). With the checklist's controllers at `none`, the controllers' poses are null and `feedback` has no `controller` (`takes.export_jsonl`): controllers left switched on still get tracked, and their poses would pass for the hands' ground truth.
  - `SHA256SUMS`.

  Compression runs at nice 19. While it runs with the headset worn, the window notes that VR may stutter a little (exports are done in the headset). Worn is judged the way `frame-job` does: `vrcompositor` runs and a `/sys/class/backlight/*/brightness` reads over 0 (SteamVR turns the panel off 5 s after the headset comes off). CPU work while in VR causes stutter. The proximity sensor is no use here: it read 9-43 with the headset sitting unworn.
- **Upload.** The Upload page uploads an export from the window, logging in included: no terminal. Below its steps it shows `UPLOAD.md` ("About uploading"), with the export's path, size and contributor id filled in.
  - **`hands/rec/validate.py`** (standard library) checks an export. The window runs it before an upload, and the maintainer runs it on each submission (`validate.py DIR [--json]`, exit status 1 on errors). It checks:
    - `SHA256SUMS`: every file listed and matching.
    - An allow-list: `manifest.json`, `calibration.json`, `device.json` and `SHA256SUMS` at the top, and `prompts.jsonl`, `poses.jsonl`, `take.json` and `sets.bin.zst` in `takes/<NN>-<section>/`. Anything else is an error, and so is a symlink.
    - The manifest's schema, keys and types, and that it matches the files.
    - The consent version is present and the contributor confirmed being an adult. The contributor id is a uuid4.
    - `calibration.json` has nothing that `session.py`'s `strip_calibration` would still remove.
    - `device.json` holds only `cv.cad_from_cal` and `head`, each `plus_x`, `plus_z` and `position` as 3 numbers (plus `cad_from_cal`'s `method`). Without it: a warning.
    - Each `sets.bin.zst` decompresses to its end, so a truncated one fails, and every set's FHSET01 header is sane: camera names, sizes, record length. Set counts and raw bytes match the manifest. Pixels aren't decoded.
    - Every jsonl line parses.
    - `session.sides` is `{"swapped": true|false|null}`, and every take's `sides.names_swapped` equals it (else an error: export again). Without `sides`, or `null`: a warning (the maintainer's check tells).
    - The total size: a warning over 15 GB, an error over 40 GB.

    Warnings cover notes kept in the export, a home folder path in the manifest, and missing `poses.jsonl` files.

    It runs on Linux and on Windows (the maintainer's PC) with Python 3.12 or later. It decompresses with Python 3.14's `compression.zstd`, else the `zstandard` package, else the `zstd` program, and handles several zstd frames in a row.
  - **`hands/rec/hub.py`** does the upload. It runs as a child process of the window (Cancel ends it), or from the command line (`hub.py [--base DIR] whoami | login | upload SESSION [--dry-run] [--again] [--json]`). It uses `huggingface_hub` (`python3-huggingface-hub` in the dev container) with the login saved in huggingface_hub's token file.
    - **`login`** is huggingface_hub's browser login (OAuth device code), the same as `hf auth login`'s default since huggingface_hub 1.x. It asks Hugging Face for a link (`https://hf.co/oauth/device`) and a short code, prints them (`{"phase": "code", "url", "code", "expires_in"}`), and waits while the person enters the code in their browser and approves. Then it saves the token, which can refresh itself. Nobody types or pastes a token, and the window never sees one. It uses huggingface_hub's own helpers (`request_device_code`, `poll_device_token`, `_save_oauth_token`), as there's no public call that hands back the code. On 2026-10-03 the code lasted 5 minutes and wasn't filled into the link. The consent screen lists the person's organizations: none are needed, as a pull request on a public dataset comes from the person's own account.

    An upload goes through these steps: An upload goes through these steps:
    1. Validate, and stop on errors.
    2. Stop if the same export was uploaded before (same `SHA256SUMS`), unless asked again.
    3. Stop while the texts are drafts, unless `FT_HANDREC_ALLOW_UPLOAD=1`.
    4. `whoami`: a read-only token is refused.
    5. `auth_check` on the dataset: a gated dataset whose terms aren't accepted gives "accept the dataset's terms first", with the link.
    6. Open the pull request first: `create_pull_request(HF_DATASET, title, description=...)`, an empty draft. Its link goes to the window right away (`{"phase": "opened", "pr_url"}`), which tells the person to plug the headset in and leave it. Record `{"repo", "pr_url", "pr_num", "started", "export_sha", "status": "started"}` in `uploads` in `session.json`. A retry of the same export goes on in that pull request while it's draft or open.
    7. `upload_folder(repo_id=HF_DATASET, repo_type="dataset", folder_path=EXPORT, path_in_repo="contributions/<contributor>/<session>", revision="refs/pr/N", commit_message=..., commit_description=...)`. The description summarizes the manifest: takes, minutes, sets, lighting, objects, controllers, the consent and tool versions, the size, and validate's warnings. Then mark the pull request open if it's still a draft (the maintainer's `list` shows open ones, so drafts are uploads still in progress).
    8. Mark the record `"status": "done"` with `uploaded`. `export_sha` is the SHA256 of `SHA256SUMS`. The file keeps its modification time, so the export doesn't count as out of date. Only finished records count as "uploaded before" (records without a status are from before 2026-10-03 and finished).

    Errors get a plain explanation: not logged in, a login Hugging Face rejects (401), a login that can't open a pull request (403), terms not accepted, dataset not found, network errors. `--dry-run` does everything except the network calls and the record, and lists what it would upload.
  - **The page** has three numbered steps:
    1. Choose the export, with its size.
    2. Log in to Hugging Face. The page shows whether someone is logged in (`whoami`). Log in runs `hub.py login`, opens the link in the browser, and shows the code large, with Copy code and Cancel. "Use another account" logs in again. A classic read-only token counts as not logged in.
    3. Upload, with a note to accept the dataset's terms the first time.

    Upload has Cancel, the phase with a progress bar (a share while the export is checked, a sweep while it's sent, as `huggingface_hub` reports no progress), the pull request's link once it's open, with "plug in the headset and leave it plugged in until this says Uploaded", and Uploaded at the end. If this export was uploaded before, the page says so, and uploading it again asks first. A stale export can't be uploaded.
  - **Staying awake:** while an export or an upload runs, the window holds a host unit, `frametop-handrec-awake.service`, running `systemd-inhibit --what=sleep:idle --mode=block ... sleep infinity`, and stops it when the last of them ends (or on quit). It's meant to keep Steam from putting the Frame to sleep with the headset off; whether Steam's sleep honours a logind block inhibitor is still to be checked on the device. Exports and uploads are done in the headset: the export page only notes that VR may stutter a little while it compresses.
  - **While the texts are drafts**, Upload stays off unless `FT_HANDREC_ALLOW_UPLOAD=1`, so the maintainer can rehearse against a private test repo. `FT_HANDREC_DATASET` overrides `HF_DATASET`. `ft-handrec --hub-dry-run` makes Upload a dry run: no network, so it isn't held back by the drafts.
  - **Rehearsal: `hands/rec/rehearse.sh [--repo ID]`** runs it all without the headset, in the dev container, in one `frame-job --local` scope when frame-job is installed. `ft-ringplay` plays 30 s of a recording into a ring in `/run/user/UID`. A tracking ft-hands that's already running is used, or one is started on that ring. `ft-handpanel --no-vr` stands in for the panel. `session.py --no-start --next-after 0.3` records a three-section test script (a prompt, a two-cue sweep, no hands) in step mode, three parts of 6 s (countdown and hold), about 360 MB once exported. Then `takes.py` exports, `validate.py` checks, and `hub.py` uploads: a dry run by default, or for real to `--repo ID` with `FT_HANDREC_ALLOW_UPLOAD=1`. It prints a summary, deletes its temporary folders (camera images of a room) and stops everything it started, Ctrl+C included. The `--no-vr` panel logs no poses, so `poses.jsonl` is missing there (a warning).

## Licensing and consent (texts in `CONSENT.md`)

- The dataset is CC BY-NC 4.0.
- Contributors also grant the maintainer (DeeJanuz) a broad, non-exclusive license to their contribution.
- Contributors confirm they're 18 or older.
- The text explains what's recorded and that nothing uploads automatically, how review works, how to withdraw (by contributor id), and that a withdrawal is purged from the repo's history.
- The texts need a legal review before the dataset launches. Until then they carry a "draft" banner, and the Upload page says contributions aren't open yet.
- The dataset repo: `HF_DATASET` in `hub.py`, `DeeJanuz/frametop-hands` (private until launch).
