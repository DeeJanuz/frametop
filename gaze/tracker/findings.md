# Findings so far

Our own eye tracker's research notes, newest sections last. They were written while it was a
separate project (frame-eyes), so they use its names: `fe-trackd` is now `ft-eyes`,
`fe-bufprobe` is `ft-eyegrab`, `fe_model`/`fe_pupil` are `eyes_model`/`eyes_pupil`, the
tools are in `lab/` (`fe-score` is `ft-eyes-score`, `fe-replaytest` is `ft-eyes-e2e`,
`fe-record` and `fe-replay` are `ft-eyes-record` and `ft-eyes-replay`), `fe-live` is the
gaze service running ft-eyes, and `captures/NAME` is `~/.local/share/frametop/eyes/captures/NAME`.

These were measured on the Frame on 2026-09-28 (SteamVR eyetracking 2.17.10), and all by
reading only.

## SteamVR's tracker process

- `eyetracking -b CDSP -w .../et_dsp_20250610_03136.weights` runs as the user (steamos),
  started by SteamVR. The user is in the `cdsp` and `spidev` groups. `ptrace_scope` is 1,
  so reading another process's fds or memory needs root (pidfd_getfd).
- Log: `~/.local/share/Steam/logs/eyetracking.txt`. Component names: `CStereoAdspCams`
  (the eye cameras come in through the audio DSP; "Set framerate 72/90"),
  `CGazeEstimatorCdsp` / `CDSPGazenet` (the neural net on the compute DSP), and
  `CEyePoseUKF L/R` (a filter per eye; "Large dt" means it had no measurement for over
  0.4 s and starts that eye over).
  - "Failed to grab cdsp input buffer" came up 5,924 times in 6 hours (about 0.3 % of
    frames at 90 Hz).
  - "Accept usercal" is its passive calibration from quick mouse clicks.
- The eye cameras aren't V4L2 devices, and there's no fastrpc node.
- Open fds that matter:
  - `/dev/spidev0.1`: modalias `spi:hid-over-spi`, role unknown.
  - Six udmabufs, all `exp_name: udmabuf`: three of 16 MiB (16777216 B) and three of
    32 MiB (33554432 B), fds 50, 51, 53, 54, 159 and 169 at the time.
  - `/dev/shm/eye-server.mmap`: its output.
  - `/dev/input/event0-7`.
  - The frames most likely arrive in the udmabufs, which it shares with the DSPs.

## The eye-camera frames (found 2026-09-28, `tools/fe-bufprobe --scan`)

- **The buffers.** The six udmabuf fds are really two buffers, three fds each: a 16 MiB one
  (inode 1) and a 32 MiB one (inode 2).
- **The frames.** In the 16 MiB buffer, eight slots sit 0x40000 apart from 0x230000, four
  per camera: slots 0-3 are camera 0 and slots 4-7 camera 1. Each slot starts with a small
  block (slot 0's holds a table of floats such as 0.00125, 4.655, 90.0, 1.0, possibly
  exposure, gain, and frame rate; the others were zero), then a 512x400 8-bit grayscale
  frame, row stride 512. The frame starts at slot base + 0x40c0 + 0x40 per slot index,
  plus one more 0x40 for camera 1's slots. Found by the dark lens-rim column lining up;
  `slot_start()` in fe-bufprobe.
- **What they show.** Infrared images, one camera per eye, dim (mean about 25-40), with a
  dark band on one side (the lens rim). One camera saw its eye at a steep angle (squashed
  pupil near the image edge, big reflections on the white); the other nearly head-on (a
  round pupil with two small glints in it). Which camera is which eye isn't known yet.
- **Timing.**
  - About 90 frames a second per camera, the two within about 0.6 ms of each other.
  - A frame lands over several milliseconds, in bursts, so a copy taken when the slot
    "stops changing" can be half old. The reliable rule: a slot is complete when its camera
    starts writing another slot.
  - Camera 0 fills its slots in turn (3, 0, 1, 2); camera 1 in a repeating order of eight
    (7, 5, 4, 6, 5, 7, 6, 4), never the same slot twice in a row.
  - `--rec` stamps each frame when its slot first changed, polling every 0.3 ms, so times
    are only good to a few ms.
  - The cameras run at 90 fps ("Set framerate 90" in the log). The first recorder copied
    a frame as soon as its camera started the next one and got 94 a second in fit1 and 106
    in a streaming test. The extras were half-written frames: a frame's last writes can land
    after the next frame starts, and a slow poll saw both at once. The recorder now copies a
    frame when its camera starts the frame after next (no slot is rewritten sooner than three
    frames), ignores late writes to the frame just finished, and saves from a separate
    thread. fit1 may hold a few percent of torn frames.
  - `--share` (2026-10-03) checks only the slot each camera writes next, from the two before
    (the orders above, learned again if they change; all four slots for 2 s after a frame turns
    up elsewhere), sleeps until 2.5 ms before the next frame is due, then looks every 1 ms with
    0.5 ms of timer slack. Against the 0.3 ms poll of all eight slots, on simulated cameras:
    207 wakeups a second instead of 1,486, 0.8% of a core instead of 3.6% (more on the real
    DMA-BUF memory), no torn or skipped frames, and a frame's start seen 1.35 ms late on
    average instead of 0.76. `--rec` still polls all slots every 0.3 ms, for its times.
  - Eye tracking stops when the headset is off ("HMD off, stopping eye tracking"), so
    recordings are empty then.
- **Which camera is which eye** (capture fit1, 2026-09-29, closing one eye at a time):
  camera 0 (slots 0-3) is the **right** eye, seen at a steep angle; camera 1 (slots 4-7) is
  the **left** eye, seen nearly head-on. Both images have the lens rim dark on the left and
  the lit face on the right.
- **Why the left eye is lost looking down:** at the keyboard, camera 1 sees only the upper
  lid and lashes. Camera 0 still catches part of the right eye. It's the camera angle, not
  the net.
- **The 32 MiB buffer.** Two 48 KiB regions (0x1522000, 0x1532000) that change every frame,
  mean bytes about 148, 99 % nonzero. Probably the net's input per eye (crops, maybe not 8-bit
  pixels). Not decoded.
- **`tools/fe-session NAME SECONDS`.** Records frames and ft-gaze's samples together, on the
  same clock (CLOCK_MONOTONIC_RAW). Each frame's nearest SteamVR sample is a median 3.8 ms
  away.

## Our first pupil finder (`tools/fe_pupil.py`, 2026-09-29)

- Threshold dark (< 30), close glint holes, drop dark regions that touch the image edge
  (lens rim, background), keep the fullest, darkest ellipse-shaped blob. Glints: spots
  >= 200 within 1.5 pupil radii. 3.1 ms a frame on the CPU, unoptimised.
- On fit1 (20 s: open, each eye closed, keyboard, up), pupil found vs SteamVR seeing the
  eye:

  | Gaze pitch | Right, SteamVR | Right, ours | Left, SteamVR | Left, ours |
  | --- | --- | --- | --- | --- |
  | below -20 (keyboard) | 79 % | 35 % | 22 % | 24 % |
  | -20 to 15 (screens, includes closed-eye time) | 91 % | 89 % | 74 % | 72 % |
  | above 15 | 100 % | 100 % | 98 % | 99 % |

  It misses the right eye looking down, where the lower lid cuts the pupil. False finds
  on closed eyes: 0.4 % right, 2.8 % left.
- A quadratic fit from pupil centre to SteamVR's per-eye gaze, held out by time block:
  median 4.8 degrees right, 3.0 left. That isn't an accuracy figure yet. fit1 has few
  distinct gaze points, it uses no glints, and SteamVR's per-eye gaze is itself off by
  several degrees. It needs a recording against known targets.

## eye-server.mmap (its output)

The file is 324,122 bytes. Only bytes 0x0-0x1f3 are used; the rest is zero. It's packed and
unaligned, so read it with memcpy. Offsets are also in `~/frametop/gaze/ft-gaze.cpp`.
On SteamOS 0.4 (SteamVR 2.18.2: the 0.4.3 beta, and 0.4.5, the release) every field from 0x157 on
sits 5 bytes later, and the counter stays at 0x38 (measured 2026-10-04, PR #26). The offsets below
are SteamOS 0.3's; ft-gaze detects which layout is live.

| Offset | What |
| --- | --- |
| 0x38 | u32 sample counter |
| 0x157 | f64 sample time, CLOCK_MONOTONIC_RAW |
| 0x15f, 0x16b | set 1: left and right eye direction (3 f32, head space, -Z forward). Filtered; both eyes always share one pitch; a lost eye keeps its yaw |
| 0x177 | set 1: 6 f32 variances (left 3, right 3; the middle one of each is shared). About 0.0005-0.002 when the eye is seen, 0.015-0.03 when it's lost |
| 0x18f | set 1 fixation point (3 f32; its length is the vergence distance) |
| 0x19b, 0x1a7 | set 2: each eye's own direction |
| 0x1b3 | set 2: 6 f32 variances |
| 0x1cb | 2 f32, 0..1: openness (0 in a blink) |
| 0x1d3 | 8 f32: left measurement x, y; right x, y (camera-relative, freezes while that eye isn't seen); then variance of left x, y, right x, y (about 2e-5 on a clear view, rising as lids or lashes get in the way) |
| 0x0-0x157 | header, plus records that look like the calibration-click channel into the tracker. Never write |

## Accuracy of SteamVR's gaze (this user, this headset)

- **Tonight's practice (71 clicks, 45 minutes):**
  - raw error: median 5.1 degrees (3.0 in the 21:23 test; it varies by session);
  - corrected at the press: median 1.5, with 1 in 10 past 3.3;
  - best smooth correction fitted to the same clicks, each predicted from the rest: 1.7-1.9;
  - weighting recent clicks more (half-lives from 20 minutes down to 1) didn't help, so
    there's no slow drift to follow.
- **Look-to-look:** two looks within 3 degrees of each other, under 5 minutes apart,
  differ by a median 1.15 degrees (3.0 when 5 or more minutes apart). Jitter within one
  look is 0.25-0.3.
- **One eye alone (set 2), against both eyes' gaze:** median 0.8 degrees over a steady
  look. Per-eye raw errors are large and opposite in yaw: at one spot, left (+8.2, +8.9)
  and right (-5.0, +5.2) degrees, both (+1.6, +7.1).
- **Losses:** the left eye was lost 57-64 % of the time looking 30-50 degrees down (at
  the keyboard, through the gap by the nose), and the right eye never. At screen height
  both were seen over 98 % of the time. Openness looking down: left 0.45, right 0.65.
  Harmless for the pointer: ft-gazed ignores looks down past the screens.

## First accuracy test against known targets (practice1, 2026-09-29)

5 minutes, 98 gaze-probe practice clicks, gaze yaw -26..25 and pitch -14..20 degrees. Truth
is SteamVR's raw gaze at the press plus the angle to the release point. `tools/fe-score.py`
fits a quadratic per method and scores each click leave-one-out. Pupil = median centre over
the frames 250-20 ms before the press; no glints yet.

| Method (85 clicks with both pupils found) | Median | 90 % |
| --- | --- | --- |
| SteamVR raw | 6.51 | 11.04 |
| SteamVR + quadratic fit | 1.52 | 3.10 |
| Ours, right pupil only | 0.74 | 1.48 |
| Ours, left pupil only | 0.62 | 1.48 |
| Ours, both pupils averaged | 0.61 | 1.13 |

SteamVR with the probe's live correction: 1.44 median over all 98. The pupil was found
before 88/98 clicks (right) and 90/98 (left). Caveats: one session with the headset
never moved (pupil-only mapping breaks when the headset slips; glints should fix that),
the truth includes the user's own drag precision, and it's offline only.

## Glints and slip (2026-09-29, practice1)

- Two IR LED reflections, a vertical pair 12-19 px apart, sit on the cornea near the pupil.
  There's no alternating illumination: the pair is in every frame the geometry allows.
  Before a click: right eye 45/88, left 54/90 (the steep right camera loses the pair on
  the white when the eye looks across). Bright skin has noise speckle above 200, so a glint
  only counts if the ring around it is dark (`find_glints`, `glint_pair` in fe_pupil.py).
- Pupil minus pair midpoint as the feature: 0.84 median, noisier than the pupil alone
  (0.61), because the pair's position is noisy.
- Slip method (`tools/fe-score.py`): where the pair is seen, the glint fit gives the gaze,
  a fit of gaze to pupil position says where the pupil should be, and the difference is
  the slip. The median over the last 30 s shifts every frame, glints or not. In-session:
  0.61 median, same as the pupil alone. The estimate stayed within 1-3 px all session.
- Simulated slip (fit on the first 49 clicks, test on the rest shifted 10 px): pupil alone
  0.62 -> 2.7-3.1; glint 0.83 unchanged; slip 0.67 unchanged. That checks the math only,
  for a pure image shift. A real re-seat also tilts and changes the distance.
- Next test: a second session after taking the headset off and on, scored with
  `fe-score.py captures/practice1 captures/practice2`.

## Live tracker (2026-09-29)

- `fe-bufprobe --share` (root) copies each finished frame into `/dev/shm/frame-eyes-cams`
  (0600, the user's); `fe-trackd` (user) finds pupils and glints and writes
  `/dev/shm/frame-eyes-gaze`; ft-gaze reads that as the source `own`. `tools/fe-live` runs
  both. The user side never touches SteamVR's buffers.
- The windowed pupil search gives the same results as the full frame (0.000 px apart on
  2000 frames per eye), at 0.4 ms instead of 1.4-2.1; with glints, about 1 ms a frame, 90 fps
  per eye.
- Replaying practice1's first minute through fe-trackd: 0.64 median at the 14 clicks
  (0.61 offline with the same calibration; in-sample, so a pipeline check, not accuracy).
  Sample-to-sample jitter 0.08 degrees; SteamVR's is 0.25-0.3.
- The calibration covers yaw -26..25 and pitch -14..20 degrees (practice1's clicks). Beyond
  that the quadratic extrapolates.

## Test 2: a second session after re-seating (practice2, 2026-09-29 15:18)

124 practice clicks over about 4 minutes, headset nudged at about 100 s. The probe stayed on
SteamVR's mmap2 as its source, but recorded our live gaze at every press. Scored with
`fe-score.py captures/practice1 captures/practice2` (median degrees):

| Method | Fit on practice1 | Fit within practice2 (leave-one-out) |
| --- | --- | --- |
| SteamVR raw | 2.86 | 2.86 |
| SteamVR + the probe's live correction | 1.37 | 1.37 |
| SteamVR + quadratic fit | 3.84 | 1.17 |
| Ours, pupil only | 14.31 | 2.86 |
| Ours, glint | 3.31 | 1.54 |
| Ours, slip | 2.68 (live: 2.87) | 1.36 |

- The re-seat moved the eyes 20-30 px in the images: pupil-only goes 14 degrees off. The
  slip correction takes that to 2.7, but no further. The rest is partly one offset (yaw
  -1.6, pitch +1.2; removing it leaves 1.43), and an offset from the previous 5 clicks
  gives 1.31, the same as SteamVR's live correction (1.37).
- Within one headset position (before the nudge, 45 clicks; after it, 68): pupil only
  1.01 and 0.99, slip 1.18 and 1.86, SteamVR with the same fit 0.91 and 1.19. So today our
  tracker is level with SteamVR within a position, not ahead of it as in practice1 (0.61
  against 1.69). One session was not enough to claim a lead.
- The slip estimate adds noise: it's worse than no correction within a position, and it
  wandered (the right eye's jumped 18 px near the end, after the clicks). It comes from the
  glint fit, which is itself only 1.5-3 degrees good, and the left eye's pair was seen
  before only 21 of 124 clicks.
- Live: fe-trackd ran 81-90 fps per eye at 2-3.6 ms a frame alongside VR (1 ms in replay);
  ft-gaze's `own` came through on every line, about 37 ms old.
- What would help: a geometric eye model (the eyeball centre from how the pupil ellipse
  changes shape, as Swirski's method and Pupil Labs' pye3d do) instead of 2-D regression,
  so that headset movement is modelled and not fitted around. practice1 and practice2
  together (re-seat plus a nudge) are the benchmark for it.

## The geometric model, and a shift taught by clicks (2026-09-29, practice1 -> practice2)

All offline: calibrate on practice1, score practice2's clicks.

- **Eyeball centre from the pupil ellipses (Swirski-style, weak perspective): worse.** The
  centre it finds is steady within a session (a few px per 50 s) and moves between the
  sessions about as the slip does, with a rotation radius of about 60 px (10-12 mm). But
  it's off from the true shift by up to 8 px (6-7 degrees) on the right eye. Correcting
  with it gave 4.3-5.8 median, against 2.7 for the glint slip. The cornea's refraction and
  where you happened to look in the window likely bias it.
- **The calibration itself carries over.** The best possible pixel shift per eye, fitted
  on practice2's own clicks with one shift per headset position (before and after the
  nudge), gives 1.18 held out (shift+scale 1.11, affine 1.07). So practice1's fit is fine
  if we know the shift; the problem was only estimating it. One shift for the whole
  session gets only 2.7-2.8, because the nudge moved the eyes again.
- **How well each estimate finds that shift (px, right x/y, before the nudge):** true
  (-0.7,-28.6), glints (+2.0,-25.6), eyeball centre (-6.9,-22.2). The glints are off by
  1-4 px, which is 1-3 degrees; the eyeball centre is worse.
- **Clicks estimate it best.** Each click says where the pupil should have been for a
  known gaze, so pupil minus that is the shift. Scored in time order with only earlier
  clicks:

  | Shift from | Median | 90% |
  | --- | --- | --- |
  | Glints only, last 10 or 30 s | 2.68-2.69 | 3.78-4.13 |
  | Last 3 clicks | 1.29 | 2.80 |
  | **Last 5 clicks** | **1.18** | 3.09 |
  | Last 5 clicks + glint slip since | 1.30-1.34 | 3.34-3.55 |
  | **Last 5 clicks, glints only to catch a jump over 3 px** | **1.22** | **2.33** |
  | SteamVR + the probe's live correction (same clicks) | 1.37 | 2.71 |

  Adding the glint slip to the clicks' shift adds its noise. Using it only to notice a
  nudge (then the shift follows the glints until clicks catch up) keeps the median and
  cuts the tail after a nudge. That's `fe_model.Shift`, and `fe-score`'s `clicks` method
  reproduces it (1.22 median, 2.33 90%).
- So on this pair of sessions, ours with click correction is slightly ahead of SteamVR
  with the probe's click correction: 1.22 against 1.37 median, 2.33 against 2.71 for the
  worst tenth. One pair of sessions, so not yet a lead (see Test 2).
- fe-trackd now works this way: the probe's calibration with the source "Own tracker"
  sends each dot to fe-trackd (`calib-point`), which fits from its own pupil history
  (`calib-fit`, which replaces the old calibration and clears the shifts), and each
  practice release sends a `click` that teaches the shift. See fe-trackd's docstring.
- **The live path reproduces it** (`tools/fe-replaytest captures/practice1 captures/practice2`
  on the 7i: a scratch fe-trackd, calibrated through `calib-point` from practice1's clicks,
  then fed practice2's clicks at the recorded pace and scored on what it published in the
  300 ms before each press). 84 of 98 dots accepted (14 had an eye in under 15 frames),
  fit 0.59 median. practice2: median 1.21 and 1.23 over two runs (offline 1.22), but 90%
  3.02 and 2.86 (offline 2.33). The tail: the first click (19.7, nothing learned yet after
  the re-seat), and two clicks at 192 s and 213 s (7.6 and 10.3) with a settled 5-click
  shift and no jump. (Offline has the same two, 6.8 and 9.3: see the next section.) The
  glint jump restarted the right eye's shift 9 times and the left's 4.

## Wide gaze, the left pupil, and false glint jumps (2026-09-29, practice2)

- **The bad clicks were all past the calibration, or right eye only.** practice1's clicks
  reach yaw 25 and pitch 20; practice2's reach 30 and 25. Offline (median / 90%):

  | Clicks | Ours | SteamVR + probe |
  | --- | --- | --- |
  | Inside practice1's range (104) | 1.09 / 1.97 | 1.37 / 2.71 |
  | Outside it (18-20) | 1.80 / 5.01 | 1.19 / 2.67 |
  | Both eyes (99) | 1.09 / 1.98 | 1.40 / 2.83 |
  | Right eye only (23) | 1.66 / 4.05 | 0.99 / 2.47 |

  A fit that goes linear past its data, more ridge, or a linear fit didn't help outside.
- **The left eye was lost at every click past about 19 degrees left.** The pupil is still
  mid-image (x 293-310 of 512), but the left camera's image is dark from x 0 to about 360,
  and the 7 px closing (which heals glint holes) joined the pupil to that background, which
  touches the edge, so it was dropped. `fe_pupil` now retries with a 3 px closing when
  nothing is found: left eye found in 97% of the frames before practice2's clicks (was
  79%; 181 of 217 frames at 20+ degrees, was 16), right eye unchanged, centres moved at
  most 0.16 px. The right eye still needs the 7 px (3 px alone: 96% against 98%).
- **Those pupils are accurate.** Within one headset position (practice2 after the nudge,
  68 clicks, leave-one-out, so calibrated out there too): left eye alone 0.70 / 1.39 below
  19 degrees left and 0.87 / 1.14 beyond; right eye 1.27 / 2.13 and 2.09 / 6.42; both
  averaged 0.85 / 1.30 and 1.18 / 5.14. Calibrated where you look, the left eye is our best.
- **But practice1's calibration has 4 clicks per eye beyond 19 degrees**, so the newly seen
  left eye extrapolates there (3.64 median alone, right 1.78), and practice1 -> practice2
  got a worse tail: 1.20 / 3.20 offline (1.22 / 2.33 when the left eye was simply lost
  there). Weighting the eyes, or leaving out an eye or a click past the calibrated range,
  didn't recover it. The fix is coverage: the probe's calibration for the Own tracker now
  puts its dots on an oval out to the `Calibration ring` angle each way (the ring was
  limited by the window's height and never reached the sides). A first try put them at
  the practice area's corners, which in a large window were too far to look at while
  facing the centre.
- **The glint jumps.** Offline (checked once per click) there were 2 per eye, 3 of 4 real
  (the next click found the shift the glints claimed). Live, bad glint pairs made the right
  eye's estimate leap by up to 68 px for under a second, 20 times between clicks, and the
  shift restarted 8-9 times. `Shift` now takes a jump only once it has held for 1 s
  (JUMP_HOLD) and ignores ones over 40 px (JUMP_MAX). Live replay: shift restarts 1 (right)
  and 0 (left), biggest leap between clicks 4.9 px, output steps over 10 degrees 86 -> 32.
  Offline with the hold: 1.19 / 3.35.
- Live replay with both changes: 94 of 98 calibration dots taken (83-84 before), practice2
  1.28 / 3.16. The tail stays until a calibration covers the practice area.
- `fe-score` now reads each capture's own `practice.jsonl` (cut from the probe's log by
  `fe-score.py --clicks`, or the first scoring on the Frame), so the 7i can rebuild features.

## Session 3 (2026-09-29 22:04-22:10, live, SteamVR driving)

The calibration and 84 practice clicks went to SteamVR (the probe's tracker toggle was
left on SteamVR), so fe-trackd got no dots or clicks and kept practice1's calibration. The
probe still logged our gaze at 79 presses. SteamVR + the probe's correction: 1.19 median,
2.61 90% (raw 2.18 / 4.30). Ours with practice1's calibration and no clicks: 12.5 median;
with a stand-in for the click shift (the median offset of the previous 5 clicks, in gaze
angles rather than per eye in pixels): 1.38 / 2.38. No frames were recorded.

## Session 4: the Own tracker driving (2026-09-29 22:12-22:22, live)

Fresh calibration from the probe with the Own tracker: 27 dots on an oval out to 20
degrees (30 of 31 attempts accepted; one had no right eye). Then 136 practice clicks, all
with both eyes, each teaching the shift. The probe logged SteamVR at 114 of the presses,
and its correction learned from the same drags, so the comparison is fair:

| At the same 114 presses, to where you let go | Median | 90% |
| --- | --- | --- |
| **Ours** | **0.59** | **1.50** |
| SteamVR + the probe's correction | 0.83 | 2.02 |
| SteamVR raw | 3.49 | 5.21 |

Ours was closer on 76 of 114. Ours at the press (what the dot showed, all 136): 0.67
median, 1.37 90%, and steady from the first 10 clicks (0.71) on; SteamVR's correction
took about 30 clicks to get under 1 degree. No click changed an eye's shift by more than
6 px. The user: "MUCH improved". No frames were recorded, and the headset wasn't nudged or
re-seated, so this is within one position; the cross-session question is still open.

## Session 5: off and on again, no recalibration (2026-09-29 22:26-22:31, live)

Session 4's calibration and shifts, headset taken off and put back on, then 132 practice
clicks with the Own tracker driving. The re-seat moved the eyes about 15 px (right) and
33 px (left) in the images.

- Before the first taught click, the glints had moved the right eye's shift to within
  about 4 px and the left's about two thirds of the way. Clicks 1-4 were still 11-17
  degrees off, and the probe refused to teach them (its 6-degree limit on a lesson), so
  the first taught click was the 5th (5.4 degrees). It restarted each eye's history as
  designed; clicks 6, 7, 8: 2.8, 1.3, 0.4.
- After that (clicks 6 on, 127, to where you let go): ours 0.58 median, 1.42 90%, the
  same as within one position (session 4: 0.59); SteamVR + the probe's correction 2.94 /
  5.73 (SteamVR raw drifted from 3.5 to 4.7 through the session). Ours closer on 117 of 132.
- Changes: the probe lets the Own tracker learn from drags up to 25 degrees
  (OWN_LEARN_MAX), and starts on the Own tracker when fe-trackd answers; its calibration
  header names the tracker. fe-trackd restarts an eye's shift history at the next click
  after frames stop for 3 s (the headset off) and after its own restart, glints or not.
  Replay regression (fe-replaytest practice1 practice2): 1.28 / 3.16, as before.

## Weighting the eyes (2026-09-29, practice2 after the nudge)

One headset position, 64 clicks with both eyes, leave-one-out: plain average 0.84 / 1.59;
left eye alone 0.73 / 1.36; right alone 1.32 / 3.05; weighted by each eye's inverse
residual variance on its own calibration 0.75 / 1.26. Not in fe-trackd yet.

## Next steps (2026-09-29, after a literature search; sources in the session report)

Ranked by expected gain for the effort, checked against our own numbers:
1. Weight the eyes by each one's calibration residuals (above: 0.84 -> 0.75 median). S.
2. A one-dot re-seat check when frames come back after a gap (Varjo recalibrates with one
   dot at every put-on): one look and press teaches both shifts before the first real
   click, instead of 11-17 degree first clicks. S.
3. Our tracker as a source for the Frametop pointer (ft-gazed): session 5 beat SteamVR
   across a re-seat. M. Done 2026-09-30 (below).
4. Record frames during live tests (fe-session), so each can be replayed. S (disk: about
   2 GB a minute).
5. A less biased glint slip estimate: ours is off by 1-4 px even over hundreds of frames,
   so it's bias, not noise; try taking out the part of the glint midpoint that follows the
   pupil (regressed on calibration data) before using it. S-M, offline first.
6. Smooth-pursuit calibration (a moving dot): dense labels out to the edge in about 20 s,
   for wider coverage. M.
7. Sub-pixel edge ellipse refit with RANSAC for the steep right eye (our weaker eye,
   1.32 against 0.73). S-M.
8. Later, if needed: learned pupil segmentation (EllSeg, RITnet: MIT) on the GPU through
   ncnn, a 3-D cornea model from the two glints, or a per-user network trained on the
   residuals across re-seats. L. Not recommended: the eyeball-centre model (tried, and our
   steep camera and +-20 degree range are outside its published conditions). PuRe,
   PuReST, ElSe, and ExCuSe are licensed for non-commercial use only.

## Quick wins from the next steps (2026-09-29, late)

- Eye weighting (1): `Calibration.spread` is each eye's RMS miss on its own calibration
  dots, and `combine` weights by its inverse square (floor 0.3 degrees). fe-trackd, fe-score,
  and so fe-replaytest use it; older calibrations get it from their saved dots. The
  22:16 calibration: right 1.48, left 1.08, so the left eye counts about twice as much.
  practice1 -> practice2 offline is unchanged (1.20 / 3.35: that tail is extrapolation).
- Re-seat check (2): fe-trackd's status says when the next click will start an eye's shift
  over; the probe then shows one centre dot, and a press on it sends that click. Checked
  on the 7i (a pending re-seat at start on both eyes, cleared by one click); the probe's
  screen for it wasn't seen (the web view didn't connect).
- Recording (4): `tools/fe-record` copies every shared frame (9 s of replay: 1620 frames,
  none dropped, all identical to the source); `fe-live --record NAME` runs it alongside.

## The Frametop pointer, and the eyes on live clicks (2026-09-30)

ft-gazed (`~/frametop/gaze`) can now use our tracker: `GAZE_TRACKER=own`, the Eye tracker
setting on the Gaze page of Frametop Input Settings. A mouse nudge before a click reaches
fe-trackd as a click. The nudge's raw gaze is one ft-gazed sent, so ft-gazed finds when
that look was, and fe-trackd keeps 12 s of pupils instead of 5, because the helper sends a
nudge up to 10 s after the look.

`GAZE_EYE` (auto, left, right) weights the eyes there, from each eye's own gaze. Replayed on
the 306 live clicks of sessions 4 and 5 (`clicks.jsonl`: each eye's pupil and shift just
before the click, so each is a fresh test):
- Each eye alone: left 0.96 median (mean 1.17), right 1.11 (1.26). The eyes' RMS misses
  were about equal (1.43, 1.46), unlike practice2's leave-one-out (0.73, 1.32).
- Both eyes: 0.65 (0.77) evenly. By the calibration's spread (the 22:16 one: left counts
  about twice): 0.63 (0.81). By each eye's RMS miss at its last 5, 10, or 20 clicks: 0.66
  (0.79-0.81).
- By share of the right eye: 0.3 gives 0.68, 0.5 gives 0.65, 0.7 gives 0.81.
- The eyes' yaw errors are correlated -0.37: they partly cancel, which is why two eyes
  beat either one by a third.

So a bias leans instead of choosing: Left or Right counts that eye twice. Auto starts
even and weights by each eye's RMS miss at its last 20 nudges, once each has 5. On
SteamVR's side the calibration's own fit picked the wrong eye (its dots: left 1.78, right
1.88; new spots: left 2.50, right 1.63), so auto learns from nudges, not the fit.
fe-trackd's own `combine` still uses the spread, for the probe.

## Valve's tracker

It can't be the starting point, legally or practically:

- **No source.** `/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking` is a
  stripped aarch64 binary. The paths left in it (`/data/src/eyetracking/eyetracklib/...`)
  are Valve's build machine's.
- **The net is just numbers.** `et_dsp_20250610_03136.weights` is 393,600 bytes of raw
  floats (about 98,000 parameters), with no header or architecture. The layer layout
  lives in the binary and in the program it loads onto the compute DSP (`CDSPGazenet`).
  Rebuilding it would mean reverse engineering both.
- **License.** SteamVR is Valve's proprietary software, used under the Steam Subscriber
  Agreement. That agreement doesn't allow reverse engineering, decompiling, modifying, or
  redistributing it, except where the law allows. `third_party_legal_notices.txt`
  covers only the open libraries it uses (Ceres, protobuf, ...), not the tracker. Putting
  their code or weights in a GitHub repo would be redistribution. (Not legal advice.)
- **Not much to gain.** Their net is small and tuned to their cameras. Improving it would
  need the same thing our own tracker needs: your eye images with known gaze, for
  training.

What we can use: its public output (the mmap, read-only), as a baseline and as labels.
Anything published and openly licensed is also fair game: papers and open-source pupil
detectors (check each one's license before using its code).
