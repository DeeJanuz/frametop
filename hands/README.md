# Hands (experimental, deferred)

Hand tracking from the headset's own cameras. It's deferred: it costs a lot of the headset's CPU and needs more work, so `install.sh` doesn't offer it and the README doesn't list it. It still builds and runs, installed by hand (below), for working on it.

It serves two things in Frametop:

- **Hand cutouts:** where your hand is between an eye and a screen, that eye sees the room through the screen (ft-screens, `screens/handcut.cpp`), so your hands show over the screens the way they do on a Vision Pro.
- **Pinches and grips:** with `POINTER_HANDS=1`, the pointer helper takes them as clicks and drags. Look at something and pinch to click it, with the eye tracker doing the looking (`gaze/`), or close your hand to press and drag what the pointer is on. See "Pinches and grips in the pointer" below.

Two programs, each a user service that stops when SteamVR does:

- `ft-camd` (`camd/`, C) borrows XRService's camera buffers and publishes the four IR tracking cameras' frames to a shared-memory ring. It runs on the host.
- `ft-hands` (`track/`, C++) finds hands in those frames with MediaPipe's palm and landmark models on ncnn, triangulates them, and publishes them. It runs in the dev container.

They don't start with SteamVR. `hands/run.sh install` builds them, gives ft-camd its capabilities, installs both services disabled, and links `hands/ft-handsctl` into `~/.local/bin`. Then `ft-handsctl on` starts hand tracking and `ft-handsctl off` stops it. `install.sh` doesn't install it.

For the cutouts alone, `hands/ft-cutouts on` starts the same two programs with ft-hands' `--no-gestures`: your hands show through the screens, and no pinch or grip is detected, so nothing clicks or drags. It runs this checkout's build as transient user units, so it needs `hands/build.sh` and ft-camd's capabilities (`hands/run.sh caps`) but not `hands/run.sh install`. It and `ft-handsctl on` stop each other's services, and it stops with SteamVR too.

```
ft-handsctl on | off            # on the Frame: start or stop hand tracking (SteamVR must be running)
ft-handsctl status              # the services, and ft-hands' last status lines
ft-handsctl log [lines]
ft-handsctl cutouts on|off|state  # ft-screens' hand cutouts, without stopping tracking
ft-handsctl gestures            # pinches and grips, live (tools/watch_gestures.py --distance)

hands/ft-cutouts on | off | status   # the cutouts only: no pinches or grips

hands/run.sh install            # build, give ft-camd its capabilities (sudo, once per build), install disabled
hands/run.sh start|stop         # start or stop the services
hands/run.sh restart            # after changing a setting
hands/run.sh status
hands/run.sh log [lines]
hands/run.sh caps               # after rebuilding ft-camd (a rebuild clears its capabilities)
hands/run.sh uncaps             # take them back, unless the Hand Recorder or the services use them
hands/run.sh uninstall          # the services, then uncaps
```

Settings in `~/.config/frametop.conf` (`FT_<name>` in the environment overrides them), read when ft-camd and ft-hands start:

- `HANDS_SWAP_SIDES=auto` (the default): ft-hands tells from the hands which side camera is which, and corrects ft-camd's names when they're backwards (see "Which camera is which" below). `1` forces them exchanged and `0` forces ft-camd's names; ft-hands still checks, and if the hands disagree it logs a warning and publishes the hands' answer as the truth (`sides.json`), so recordings are labelled right. The example config said `0` until 2026-10-05; `scripts/conf-migrate.sh` (run by `install.sh` and `hands/rec/install.sh`) turns that untouched line into `auto`.
- `HANDS_CPUS=5,6,7`: the CPUs the model threads run on (below).
- `HANDS_CAMERAS` (`auto`), `HANDS_BRIGHT` (`all`), `HANDS_BRIGHT_ON` (40), `HANDS_BRIGHT_OFF` (25): which cameras ft-hands tracks with, as `--cams`, `--bright`, `--bright-on` and `--bright-off` (see ft-hands). `HANDS_CAMERAS=mono` also keeps ft-camd off the colour cameras.
- `HANDS_COLOR_LEFT` (`color_video0`), `HANDS_COLOR_CROP` (`subtract`): how the colour module's calibration maps onto its images, as `--color-left` and `--color-crop`.

The pointer helper's `POINTER_HANDS` and `POINTER_PINCH_*`/`POINTER_GRIP_*` settings are in "Pinches and grips in the pointer" below.

Files, all in `/run/user/UID/frametop-hands/` (private to the user; not `/run/user/UID/frametop/`, which the desktop session deletes whenever it starts):

| File | Written by | Layout | Read by |
| --- | --- | --- | --- |
| `cam-ring` | ft-camd | `camd/fhring.h` | ft-hands, `tools/ring.py` |
| `hands` | ft-hands | `include/fh_hands.h` | ft-screens (`screens/handcut.cpp`) |
| `gestures` | ft-hands | `include/fh_gestures.h` | the pointer helper (`pointer/helper/ft-pointer.cpp`), `tools/watch_gestures.py` |

The source keeps the `fh_` names and magic strings of frame-hands, the project it started as, so recordings made with it still work.

## ft-camd

XRService owns the headset cameras. ft-camd borrows its DMA-BUFs read-only with `pidfd_getfd`, the same way FrameEyeCameraFeed does. It never touches XRService's V4L2 descriptors. `discovery` in `camd/xrcams.c` is adapted from FrameEyeCameraFeed (MIT, see `camd/LICENSE.FrameEyeCameraFeed`).

Polling buffers for changes can catch a frame while the camera is still writing it. Instead, ft-camd listens to the `v4l2:v4l2_dqbuf` tracepoint, which fires when XRService takes a buffer. It gives the buffer index, the sequence number and the capture timestamp. ft-camd learns which DMA-BUF holds each V4L2 index by watching which buffer changes at each dequeue:

- Right after XRService allocates its buffers, the mapping is allocation order.
- After XRService restarts streaming, the order is shuffled, and the mapping is learned index by index.
- The two upper cameras share one run of buffers. For them, only allocation order can tell the cameras apart.
- It also re-maps an index on the fly when its buffer holds no new frame.

**Privileges.** Setting up needs three things. `pidfd_getfd` on XRService needs `CAP_SYS_PTRACE`, because the Frame has `ptrace_scope=1`. The system-wide tracepoint needs `CAP_PERFMON`, because `perf_event_paranoid` is 2. Its format files are root-only, which needs `CAP_DAC_READ_SEARCH`. `hands/run.sh install` gives the binary those capabilities with `sudo setcap`. File capabilities need a filesystem mounted without `nosuid`. The Frame's `/home` (ext4) has no `nosuid`. ft-camd drops them all once it has set up, before it reads a frame, and then runs as you. XRService runs as you too. It also runs under `sudo`, for trying it by hand, and then drops to the user who ran sudo. It reads nothing from the ring's readers.

The ring is mode 0600, in a folder only you can write. Frame handling:

- Only complete, bright frames are published. The cameras alternate a normal exposure with a near-black one, so each camera gets 30 of its 60 fps.
- A copy torn by the camera overwriting the buffer is dropped.
- Each copy takes about 0.1 ms, and a cache sync about 0.15 ms.

Options:

- `--dark R`: a frame dimmer than R times the camera's recent brightest counts as near-black. Default 0.4.
- `--with-dark`: also publish the near-black frames, as extra ring cameras flagged `FH_CAM_DARK`. They show only light sources, so they're no use for hands.
- `--with-color`: also publish the two Arcturus colour cameras, flagged `FH_CAM_COLOR`. The service leaves it off and runs the mono cameras only (see "Known issues"). To try the colour cameras, add it to `ExecStart` in `hands/frametop-camd.service` and run `hands/run.sh install` again; ft-hands then picks the cameras by the light. Each is the luma of the 10-bit frame's valid 1972x2464 (the top 8 bits), at half size (`--color-scale 2`: 986x1232). They run at `--color-idle` (2 fps), enough for ft-hands to tell how bright it is, until a reader asks for more in `/run/user/UID/frametop-hands/color-fps` (ft-hands writes 30 while it tracks or records with them), up to `--color-fps` (30; the cameras run at 60). `HANDS_CAMERAS=mono` leaves them out. Frames that carry the module's warped half-size copy are dropped. Their `capture_ns` is on the colour module's clock (2.2 s off the mono cameras' on 2026-09-29), so line them up with the mono cameras by `dqbuf_ns`. Each frame costs about 0.65 ms of cache sync and 1.1 ms of decoding, so both cameras at 30 fps take about 11% of a core.
- Each mono camera's latest near-black frame's mean goes in the ring (`dark_mean`): a short fixed exposure, so it follows the room's IR light, sunlight above all.
- The ring holds 8 cameras: 4 mono, plus 4 dark twins or 2 colour cameras.
- Colour isn't reliable yet. In the lit-room test of 2026-09-30, the colour cameras kept losing their buffer mapping while the headset was worn: 30 frames in a row looked unchanged, the camera relearned, and after 5 relearns ft-camd exited. Each relearn probed all 32 colour buffers, a whole-buffer cache sync each, which also made the mono cameras miss frames. Runs with the headset idle had none of this. So the passthrough compositor may be writing into the colour buffers while Room View shows. Since then a colour camera never takes the mono ones down: it probes at most 4 buffers a frame, and one that goes stale twice in a row is paused (10 s, doubling up to 160 s) and learned again, without ft-camd exiting. Whether a frame is new is judged on the luma rows only: the chroma after them hardly changes in a lit room.
- `FT_CAMD_DEBUG=1` in the environment: at each stale colour frame, ft-camd logs to stderr the camera, the frame's time, V4L2 index and sequence number, and, for every candidate buffer, how many of its sampled words changed, in all and in the last eighth of the samples.
- `--sensor S`: only the mono cameras whose sensor name contains S.
- `--status S`: a status line every S seconds (0: never).

It exits when XRService exits, or when a camera's buffers keep going stale, which means XRService has reallocated them. The service starts it again, and it attaches to the new buffers.

**Which camera is which:** video9 is `slam_right` (sensor `og01a1b 4-0036`), video13 is `slam_left` (`4-0060`), video6 is `upper_left` and video7 is `upper_right`; without the colour module the side pair is on video0 (`slam_right`) and video3. XRService says so in its log: `Found camera 'slam_left': ... v4l_subdev=/dev/v4l-subdev30` names each sensor, and each `TrackingCameraInit` line gives the device and subdev it opened. ft-hands and `camcheck.py` name the devices that way. Until 2026-10-05 they named them by the `TrackingCameraInit` index instead, which is only the order XRService opens them in (index 0 was `slam_right` on every start logged), and ft-camd bound each buffer queue to a device by the order of XRService's file descriptors, which changes when XRService restarts its cameras. The two mistakes made the side names come out swapped on most starts and right on some. ft-camd now asks each device which buffers it holds (`VIDIOC_QUERYBUF` names the descriptor XRService queued at each index) and only falls back to the order if that fails. With the names swapped, every hand is seen by one camera only, at the wrong depth, and the hand holes land beside the hands. ft-hands still checks by itself, in case (`track/sides.h`, `HANDS_SWAP_SIDES=auto`):
- Whenever a hand's landmarks are found in two cameras at once (one of them a side camera), it intersects the rays through the 21 landmarks twice: once with the calibrations as named, once with the two side cameras exchanged. The same hand seen the right way meets within a few mm, in front of both cameras and as far away as its size says. The wrong way misses by centimetres or meets behind a camera.
- With the names wrong, the tracker never gets such pairs on its own: it hands the hand over to where the wrong calibration puts it and finds nothing there. So 5 times a second while undecided, the check places a tracked hand in 3D under the other naming and runs the landmark model where that puts it in the other side camera.
- It decides after 10 votes one way and none the other, or 20 with at most a fifth the other way, over at least 1 s. That takes about 1-2 s of hands in view. If the names are backwards, it exchanges them; the tracked views move with their images. Then it checks once more, more strictly.
- The log says what it found (`side cameras: SWAPPED, now exchanged after 3.2 s (votes ...)`). So does `/run/user/UID/frametop-hands/sides.json`, which the hand recorder reads. Recordings get a `DIR/sides.json` (hands/rec/sides.py has the rules).
- `--record-only` can't tell (it tracks nothing): it records ft-camd's names unless `--sides 0|1` says otherwise.

`tools/check_sides.py --ring` is the independent check from the scene (ORB matches meeting under each naming): exit 0 as named, 3 swapped, 2 can't tell. `--pair upper` checks the upper pair the same way: in every recording so far (3 XRService starts, both side namings) the upper pair was named right. The colour cameras are video3 (`arcimx616 0-0010`) and video0 (`0-001a`); which of them is `passthrough_left` in the module's calibration is for `tools/check_color.py` to settle, on a recording with texture in view.

## ft-hands

```
hands/build/ft-hands                  # status every 5 s; Ctrl+C to stop
hands/build/ft-hands --int8           # the 8-bit models (models/ncnn/*-int8.ncnn.*)
```

Run it in the dev container (`distrobox enter dev -- ...`). It reads the factory calibration from `/persist` (`/run/host/persist` in the container).

Options:

- `--threads N`: model threads, pinned to the `--cpus` list. Default 3.
- `--cpus LIST`: CPUs for the model threads and the main loop. Default `5,6,7` (`HANDS_CPUS`). SteamOS starts user processes on CPUs 0-4, and XRService's head tracking runs on 2-3. With the headset on, a step took 8.4 ms on 5-7 against 13.2 ms on 2-4, and SteamVR's frame timing didn't change (2026-09-29, three rounds of the same replayed frames).
- `--contrast MODE` or `PALM/HAND`: how crops are equalized before the models see them: `clahe[:CLIP]`, `none`, or `stretch` (1st-99th percentile). Default `clahe:2/none`. In the dim recording, CLAHE let the palm search find about 10% more hands, but it made the landmarks jitter more (published median 6.9 mm, against 6.0 mm with plain landmark crops).
- `--sides auto|0|1`: which side camera is which (`HANDS_SWAP_SIDES`, see "Which camera is which"); `--swap-sides` is `--sides 1`.
- `--seconds N`: stop after N seconds.
- `--status S`: how often to print status, in seconds.
- `--models DIR`: where the models are.
- `--nice N`: niceness. Default 5, so the VR stack wins contested CPUs.
- `--no-publish`: don't write the hands and gestures files.
- `--no-gestures`: hands for the cutouts only. No pinch or grip detection, so nothing reaches the pointer and a closing hand doesn't raise the rate to 30 Hz. The gestures file is removed at start. `ft-cutouts` runs it this way.
- `--record DIR`, `--record-for S`, `--record-hz N`: save every frame set for S seconds (default 120) to `DIR/sets.bin`, or at most N sets a second with `--record-hz` (the hand recorder uses 10). Every set is about 80 MB/s. Sending the tracker SIGUSR1 (`pkill -USR1 -x ft-hands`) starts a recording in `~/.local/share/frametop/hands/rec-<time>` without a restart. Recordings are images of your hands and room: they stay on the headset unless you move them.
- `--record-only`: record without tracking or publishing, so it can run beside the live tracker. Give it `--record DIR`, since SIGUSR1 would reach both trackers. With `ft-camd --with-dark`, recordings also hold each camera's newest dark frame as `<name>_dk`, which doubles the rate. With `--with-color`, each colour camera's newest frame is saved with every set, as `color_video<N>`, which adds about 70 MB/s. Run the recorder at normal I/O priority: idle I/O priority stalled a 165 MB/s recording.
- `--keep-presence P`: the landmark presence a tracked view needs to stay tracked. New views always need 0.5. Default 0.5. Lowering it to 0.2 barely helped in the bright recording, because lost hands drop to near-zero presence.
- `--ring PATH`: read frames from another ring, such as `ft-ringplay`'s.
- `--cams auto|mono|color|all` (`HANDS_CAMERAS`, default `auto`): which cameras to track with. The mono IR cameras light the hands themselves and track well in dim rooms, but in bright light they expose for the room and the hands come out dark. The colour pair is the other way round. `auto` goes by the colour frames' mean brightness: at `--bright-on` (`HANDS_BRIGHT_ON`, 40) or over for 2 s it tracks with `--bright` (`HANDS_BRIGHT`: `all`, every camera, the default, or `color`), and under `--bright-off` (`HANDS_BRIGHT_OFF`, 25) for 2 s with the mono cameras again. A dim evening room read 9. The switch is logged (`cameras: mono -> all (...)`), and the status line gives the colour level, the mono cameras' ambient IR, and how many steps had colour frames. Colour frames arrive on their own schedule, so a step holds the mono set, the colour pair, or both, and views wait in their camera for its next frame. With no colour cameras in the ring (ft-camd without `--with-color`, as the service runs it), ft-hands tracks with the mono cameras whatever this says.
- `--color-left NODE` (`HANDS_COLOR_LEFT`, `color_video0`) and `--color-crop subtract|none` (`HANDS_COLOR_CROP`, `subtract`): how the colour module's calibration maps onto the images. Not settled yet: `tools/check_color.py` on a recording with a lit, textured view tells.
- `--grip-begin R`, `--grip-end R`: the grip detector (below). Defaults 1.2 and 1.45.
- `--gesture-log`: print what the pinch and grip detectors measure, 10 times a second: each hand's thumb-to-index distance (world and triangulated), its palm-down reading and its finger curl.

**Gestures** (`/run/user/UID/frametop-hands/gestures`, `include/fh_gestures.h`), for the pointer helper:

- A pinch: the thumb and index tips within 2 cm, ending past 3.5 cm (see "Pinch" below).
- A grip, a closed hand: every finger's tip nearer the wrist than 1.2 times its knuckle is (from the model's 3D hand, so hand size doesn't matter), ending when they open past 1.45 on average. It begins only on a hand seen open within the last second (closing it is the gesture), with the palm at most 35 degrees below straight ahead and at least 15 cm in front of the eyes, and not with the thumb within 3 cm of the index tip (that's a pinch with the other fingers curled). A grip ends a pinch on the same hand, as lost. In the 2026-09-30 lit recording (no deliberate fists), the checks cut false grips from 14 to 6, all with the hands on the desk while looking down at it. The pointer helper ignores grips that begin more than `POINTER_GRIP_BELOW` (0.35 m) below the eyes, which it can tell and ft-hands can't.
- `tools/watch_gestures.py --distance` shows both live; `ft-handreplay --timeline` logs them and each hand's finger curl.

The status line also says how often a hand was on each side (by where the wrist is), and why views and hands came and went: views lost (the landmark model stopped seeing the hand), handoff misses (a crop projected from the hand's 3D position found nothing), duplicates, splits (two views disagreed in 3D), and hands created, merged and forgotten.

### Scheduling

- Each hand is tracked in its best two cameras, the way MediaPipe tracks: the landmark model runs on a crop placed from the previous landmarks, with no palm detection.
- A hand seen in too few cameras is projected into the others through the calibration. Where it lands well inside a camera, that camera gets a crop to try. This is how a hand raised out of the side cameras reaches the upper ones.
- The palm detector runs only while fewer than two hands are tracked, at most 5 times a second, on a few zoomed tiles per search. Tiles are picked in proportion to how likely hands are there. Each tile is turned so the expected shoulder-to-hand direction points up.
- Frame sets are processed at 30 Hz while a hand moves faster than 0.25 m/s (or a pinch is down or closing), at 15 Hz otherwise, and at 5 Hz while no hand is in view.

### 3D

- **Two or more views:** each landmark is triangulated from the camera rays, weighted by the model's presence score. The median ray distance is reported as the residual.
- **Pairing views across cameras.** The side cameras sit side by side, so two hands next to each other at the same height fall on the same epipolar lines, and rays to two different hands can nearly meet close to the cameras. That made phantom hands 12-15 cm in front of the eyes, which tore holes through the screens. Each step now scores every way of pairing the views in two cameras and keeps the best. A pair scores well when its rays meet, when each view's apparent size matches the triangulated distance, and when the model calls both the same hand. The size check uses a fixed prior: with the model's average hand, clean pairs measure 0.71-1.51 times the one-view distance, and mismatched pairs mostly far less.
- **One view:** depth comes from the model's metric world landmarks, their spread across the palm against the angle it covers in the image, scaled by the user's hand size (learned while two views are available). That distance is off by 10-30% and wanders about 10% between frames, so a hand that drops to one camera keeps its last distance and drifts toward the one-view guess by 10% a frame.
- **Smoothing.** The published landmarks go through a One Euro filter: it smooths hard while the hand is still (tracking noise is several mm per frame) and hardly at all while it moves fast. The palm speed that sets the update rate is the filtered one; the raw speed read about 0.25 m/s from noise alone.
- **Capsules.** Forearms follow the hand's own axis, and nothing within 12 cm in front of the eyes is published.

How good the depth is, measured from recordings (2026-09-30, `--depth` below): the two lower cameras see the hands about 77% of the time, a lower and an upper camera 7-12%, and one camera 12-15%. Depth is the noisy direction. With the lower pair, it jitters 4-6 times as much as sideways position (published: 3-7 mm against 1-2 mm). The one-camera guess is a median 2-6 cm off. When a camera drops out, drifting 10% a frame toward that guess is worse than keeping the last distance (after 0.5 s a median 23-30 mm off, against 11-12 mm).

## Pinch

ft-hands detects a pinch per hand (`track/pinch.h`) and publishes it to the gestures file. The layout, and how to read it without missing quick taps, is in `include/fh_gestures.h`.

- A pinch begins when the thumb and index tips come within `--pinch-begin` (default 0.020 m). It ends when they open past `--pinch-end` (0.035 m) for 2 processed frames in a row, or when the hand stays lost for 0.25 s (flagged lost).
- The distance comes from MediaPipe's world landmarks: the model's own 3D hand pose, averaged over the hand's views, at the user's hand size. `--pinch-triangulated` uses the triangulated tips instead. On two recordings without deliberate pinches, the world landmarks came under 2 cm in 0.2-1% of frames, against 3.3-4.5% for the triangulated tips. In the dim recording, typing still gave 2 pinches a minute (see the next point).
- `--pinch-palm-down MAX` holds back pinches begun with the palm facing down (MAX is the palm normal's share of the head's up axis). The default, 1, turns it off. A close held back that way has to open again before a pinch can begin. Typing curls the thumb onto the index: in the lit recording of 2026-09-30, typing on a keyboard in the lap began 23 pinches in about 2 minutes, all with the palm facing down (0.69-1.00), while the 26 deliberate ones read 0.00-0.50. But in the headset, deliberate pinches with the hand raised in front read 0.90-0.99 too, so the limit is off. Typing is caught by the pointer helper instead: the input relay tells it when you press a key, and no pinch begins within `POINTER_PINCH_TYPING` of one.
- A hand a pinch is down on stays with that side until the pinch ends. The left/right call is a running average of the model's, and when it flipped mid-pinch, the other side took the same hand and both sides pinched at once.
- The pinch point is between the index and middle knuckles, which hold still while the fingers open and close. The tips' midpoint moved 1-2 cm as a pinch opened, which dragged every release off its press. A drag is the pinch point now, minus where it was when the pinch began, both turned into the room with the HMD pose at their capture times.
- `tools/watch_gestures.py` prints begins, ends and drag offsets live, and `--distance` prints each hand's distance.

## Pinches and grips in the pointer

With `POINTER_HANDS=1`, the pointer helper (`pointer/helper/ft-pointer.cpp`) reads the gestures file every frame. It's off by default.

- **Pinch to click.** In gaze mode a pinch works like the mouse's press: the pointer stops where the gaze put it, and the click comes when the pinch opens, where the pointer is then. A quick tap clicks where you looked. Held, the pinching hand moves the pointer to correct the gaze, and the correction is a lesson for the gaze tracker, as with the mouse.
- **Without gaze mode,** a pinch is a real press, like the mouse's button: pressed when it closes, released when it opens, and while it's held the hand drags the pointer. A tap is still a click where the pointer is.
- **Grip to drag.** Closing the hand presses where the pointer is, the hand moves the pointer, and opening the hand releases. So a title bar moves its window, a panel's grab bar carries the panel, and text gets selected.
- A pinch ended by losing the hand, or by a grip taking over, doesn't click.
- The hand's movement is taken in the room, from where the eye was when the gesture began, so turning your head doesn't move the pointer. The first gesture while the pointer is off only wakes it. Gestures are ignored in a VR game (unless the dashboard is up), with the headset off, and while the mouse's button is held.

Settings in `~/.config/frametop.conf`:

- `POINTER_HANDS` (0): 1 turns pinches and grips on.
- `POINTER_PINCH_GAIN` (0.5): a held pinch moves the pointer this many times the hand's angle, seen from the eye. Under 1 gives precision.
- `POINTER_PINCH_DEADZONE` (1.5): how many degrees the pinching hand moves before the pointer does, so a tap's jitter and the pinch point shifting as the fingers close don't move it.
- `POINTER_GRIP_GAIN` (1): a grip moves the pointer this many times the hand's angle.
- `POINTER_GRIP_BELOW` (0.35): grips that begin more than this many metres below the eyes are ignored, because hands resting on a desk curl like a loose fist. Pinches have no such limit: deliberate ones sat 0.35-0.45 m below the eyes with the elbow resting.
- `POINTER_PINCH_TYPING` (1): no pinch begins within this many seconds of a key press, because typing touches thumb to index.

## Recordings

`hands/build.sh --tools` also builds the offline tools.

`ft-handreplay DIR` runs a recording through the tracker with the live scheduling and reports how well it kept the hands: hands per set, left and right coverage, track lengths, pinches, jitter, and the same reasons as the status line.

```
hands/build/ft-handreplay ~/.local/share/frametop/hands/rec-20260929-120000 --cost --oracle 10 --timeline /tmp/tl.txt
```

- `--cost`: instead of timing the steps, charge each round of model calls what it typically costs live (10 ms landmarks, 18 ms palms), so results repeat exactly.
- `--oracle N`: every N-th set, also search every tile of every camera, and report how often the tracker had the hands that full search could find.
- `--slow F`: live, the tracker skips sets that arrive while it's busy. Replay counts each step's time times F as busy (default 1; the headset is busier live).
- `--timeline FILE`: a line per processed set and hand, with pinch events and distances.
- `--cams mono|color|all`: which cameras to track with (default `mono`). `color` tracks with the Arcturus pair alone, for comparing it with the IR cameras on the same recording. It needs a recording made with `ft-camd --with-color`. `--color-left NODE` (`color_video0` or `color_video3`) and `--color-crop subtract|none` say how the module's calibration maps onto the images; `tools/check_color.py` finds out.
- `--depth FILE`: a line per hand per processed set for `tools/depth_report.py`, which measures the depth without ground truth: how the hands were seen, the noise along the line of sight against across it, each camera's one-view distance against the triangulated one, and what a camera dropping out would do.
- The pinch, contrast and presence options are ft-hands'.

`ft-ringplay DIR --ring PATH [--from S] [--to S] [--loop]` publishes a recording into a ring file in real time, as ft-camd would, so `ft-hands --ring PATH --no-publish` runs the same frames run after run. It needs no privileges, and it skips the dark frames.

## Tools

Python, with NumPy and OpenCV. `setup/dev-container.sh` doesn't install them, because Fedora's `python3-opencv` pulls in over a gigabyte; in the dev container, run `sudo dnf install python3-numpy python3-opencv` once. Off the Frame, `FRAME_JOB_DEVICE_ROOT` can point at a folder with copies of the headset's calibration files.

- `tools/check_sides.py --ring` (or a recording, a sets file, `--pair upper`, `--calib DIR`, `--json`): are the side cameras named right, from the scene? ft-handreplay's `--sides file|0|1|auto` replays with DIR/sides.json's names (the default), as recorded, exchanged, or as auto decides, and reports what the side check found and when.
- `tools/check_color.py REC`: how the colour module's calibration maps onto its images.
- `tools/show_set.py REC`: a recording's frame sets as images.
- `tools/watch_gestures.py [--distance]`: pinches and grips, live.
- `tools/depth_report.py DEPTH`: the depth measures above.
- `tools/cut_sets.py REC OUT [--sets N | --at I,J,...]`: copies a few frame sets (by default 8, spread evenly) out of a recording into a small one, to look at or check elsewhere without moving gigabytes. Plain Python, so it also runs on the Frame's host.
- `tools/convert_models.py`: how `models/ncnn` was made from the OpenCV Zoo ONNX ports of MediaPipe's models (see `models/NOTICE`).

To try the hand cutouts without restarting the desktop, `screens/build/ft-handtest [--distance m] [--width m] [--seconds s]` (built by `screens/build.sh`, run in the dev container, with hand tracking on) shows a test panel of its own, a light grid 1 m wide and 0.8 m ahead by default, and cuts your hands out of it the way ft-screens cuts them out of the screens.

## Camera check

Hand tracking needs all four mono cameras and the headset's IR light. With the Arcturus colour module attached, SteamVR's XRService loads an FPGA image ("VCINT") onto the module every time it opens the cameras: when SteamVR starts and after every wake. When that load fails, XRService runs only the two side cameras, the frames come out darker and noisier, and ft-hands finds no hands at all. It happened on 2026-10-02 at 17:02, after the headset slept; the hand recorder then said "I can't see your hands" for a whole session.

`hands/camcheck.py` (system Python, standard library) tells whether the four cameras run: `ok`, `degraded: upper cameras and IR light off (VCINT FPGA failed to load)` (or another `degraded:` reason), or `unknown` (SteamVR not running, the cameras closed while the headset sleeps). It prints the log lines and other evidence it used; `--json` is for programs; the exit status is 0, 1 or 2. It reads:
- the running XRService's log (`~/.local/share/Steam/logs/xrservice.txt`): the last camera start (from the FPGA check to the next "Closing tracking camera interfaces"), its VCINT result, `Upper cameras FPGA interleaving support: N`, `Created N tasks (T tracking, P passthrough)` and the `TrackingCameraInit` lines. A wake that works prints no "Created N tasks", so an older one doesn't count;
- which `/dev/video*` XRService has open (`/proc/PID/fd`; video9 and video13 are the side pair, video6 and video7 the upper pair). Only on the host: the dev container can't read another process's open files, so there it's skipped;
- ft-camd's ring header, when it runs: how many mono cameras it publishes.

The hand recorder runs it before a session (DESIGN.md, "Camera check"). `hands/tests/test_camcheck.py` runs it on the 2026-10-02 log cut at several points, and on made-up logs.

### The watcher (off by default)

`hands/ft-camwatch` follows the XRService log (a stat every 2 s, reading only what's new). On a VCINT failure it posts a notification in the Frametop desktop, on the desktop's own D-Bus, found through its plasmashell as `decoration/apply.sh` does. With `CAMWATCH_AUTO_RESTART=1` in `~/.config/frametop.conf` it also restarts SteamVR, but only:
- while the headset isn't worn (frame-job's check: `vrcompositor` runs and a `/sys/class/backlight/*/brightness` is over 0), and after it has been off for `CAMWATCH_IDLE_S` (60);
- with no app Steam launched (`SteamLaunch AppId=N` in a process's arguments; `CAMWATCH_IGNORE_APPIDS` lists ids that don't count) and nobody on the remote desktop (an established connection to the VNC port, `VNC_PORT`, 5900);
- once per failure, and not again within `CAMWATCH_COOLDOWN_MIN` (30) of the last automatic restart. It remembers both in `~/.local/state/frametop/camwatch.json`, so its own restart doesn't reset them.

It logs every decision to the journal. `ft-camwatch --once` prints the state and what it would do, and does nothing; `--dry-run` keeps watching without acting. `hands/frametop-camwatch.service` is the unit (a template, `@REPO@` as in the others; nothing installs or enables it yet). It isn't `PartOf=steamvr.service`, so it outlives the restart it asks for. `CAMWATCH_NOTIFY=0` turns the notification off. `hands/tests/test_camwatch.py` tests its decisions with made-up inputs.

### What a SteamVR restart does to Frametop

Read from the code on the experimental branch, not tried live:
- ft-screens quits when SteamVR does: on `VREvent_Quit` it ends its Wayland display (`screens/vr.cpp`, `ft_vr_poll`; `screens/compositor.c`, `handle_vr_event`). It never connects to SteamVR again: `ft_vr_init` runs once, at its start.
- KWin runs nested in ft-screens, so the Frametop desktop ends with it, every window in it too (the hand recorder's as well). Its unit, `frametop-desktop`, is a transient `systemd-run` unit with `Restart=no`, so the desktop doesn't come back by itself: start it again (Desktop in the library, or `desktops.sh start`).
- When the unit stops, systemd ends what's left in it. `session/keep-apps.sh` moves programs started in the desktop out of the unit first, but only `desktops.sh stop` runs it; here they stop too. Programs in the dev container (ft-screens, the hand recorder) are in the container's cgroup and end when their Wayland connection goes.
- The units that are `PartOf=steamvr.service` restart with it: `frametop-camd`, `frametop-hands`, the pointer helper, gaze and power, and the hand recorder's own transient ft-camd and ft-hands units.

### Verified, and what's a guess

Verified, from the XRService logs of 2026-10-01 and 2026-10-02 and the running system:
- The failure's log lines and its effect: "Failed to load VCINT FPGA image when passthrough cameras are connected", interleaving support 0, "Created 4 tasks (2 tracking, 2 passthrough)", and only video9 and video13 opened. At 19:38-19:59 XRService held only those two of the four (plus video0 and video3), and ft-camd published two mono cameras.
- A wake's load can work and can fail. Both wakes in the logs started from an FPGA that answered nothing ("ERROR/UNKNOWN"): the one at 2026-10-01 16:39 loaded VCINT, the one at 2026-10-02 17:02 failed ("FPGA config_done signal did not assert").
- After a reboot the FPGA reads PASSTHRU and SteamVR's start loads VCINT (2026-10-01 21:53, 2026-10-02 13:39).
- A SteamVR restart within a boot found VCINT still loaded and loaded nothing (2026-10-01 15:27): XRService checks the FPGA when it starts and loads only when it must.

Guesses, not tested:
- **Whether a SteamVR restart fixes it.** After a failed load the FPGA doesn't answer, so a new XRService would run the same load a wake runs, which has worked once and failed once. It's never been tried after a failure. If it doesn't help, only a reboot is known to work (the FPGA comes up as PASSTHRU, and the load at SteamVR's start has worked both times).
- That the IR light is off because of the FPGA: the frames are darker and the illuminator ring isn't seen, and the FPGA loader lists a `room_led_en` pin, but nothing shows the light's state directly.
- That a sleep and wake (taking the headset off long enough) would retry the load too: it should, since every wake loads VCINT, but no failure has been followed by a wake yet.
- How the Frametop desktop behaves on a SteamVR restart (above): read from the code only.
- That Steam-launched apps carry `SteamLaunch AppId=N`: from Steam on other Linux systems; no VR game has run on the Frame to confirm it.

## Build

`hands/build.sh` builds in the dev container on the Frame, into `hands/build/`, with `hands/Makefile`. The first build fetches ncnn at a pinned tag (`NCNN_TAG` in the Makefile) and builds it into `hands/build/ncnn`, which takes a few minutes; `NCNN=DIR` points at an ncnn install already built instead. ft-camd is linked statically, because it runs on the host, which has an older glibc than the container.

## Known issues

- **The side cameras could come out swapped** (fixed 2026-10-05, see "Which camera is which"). If they still do, ft-hands corrects it from the hands (`HANDS_SWAP_SIDES=auto`, the default): until it has seen about 1-2 s of hands in both namings' reach, the cutouts may sit beside the hands. ft-camd's log says `bound by VIDIOC_QUERYBUF` for each camera when its buffers were matched exactly.
- **The colour cameras can't be used while the headset is worn.** The colour module then writes only a half-size image into the top-left quarter of its buffers, and ft-camd drops those frames. So the service runs the mono cameras only, and tracking in bright light, where the mono cameras see dark hands, doesn't get the colour pair's help.
- **The colour calibration mapping isn't settled.** Which colour camera is `passthrough_left` (`HANDS_COLOR_LEFT`) and how the module's crop applies (`HANDS_COLOR_CROP`) still need `tools/check_color.py` on a recording with a lit, textured view.
- **Depth when one camera loses the hand.** A hand seen in one camera drifts 10% per update toward the one-camera depth guess (`kMonoDepthGain`, 0.1, in `track/tracker.cpp`). In the 2026-09-30 replays that was worse than keeping the last distance (see "3D" above). A smaller gain, such as 0.02, is the next thing to try.
- **Pinches aren't reliable enough for everyday use yet.** That's why hand tracking stays off until `ft-handsctl on`, and `POINTER_HANDS` is 0 by default.
- **SteamVR can leave the upper cameras and the IR light off after a wake**, and then no hands are found. See "Camera check" above: `camcheck.py` tells, the hand recorder won't start a session, and the fix is a SteamVR restart or a reboot.
- **Floating windows don't get hand cutouts.** Their panels show crops of the client buffer, which the cutouts' side-by-side buffer doesn't match (`screens/vr.cpp`, `UpdateCutouts`).
