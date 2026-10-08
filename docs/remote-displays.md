# Remote displays (exploration)

Status: exploration. Spike S1 is done; nothing in Frametop has changed yet. Branch `remote-displays`, written 2026-10-06.

The idea: show desktops streamed from other machines as Frametop panels that behave like the native screens. They get placement, curve, pinning, layouts, lasers, gaze, and attention-based frame rates. The target is up to 5 remote displays from 2 hosts, with no more headset overhead than 5 native screens.

## What the hardware gives us

These were checked on the Frame (SteamOS kernel 6.18, SM8650 / Snapdragon 8 Gen 3, Mesa 26.3 Turnip on Adreno 750).

- **Decoder.** `/dev/video22` (`/dev/video-dec0`) is `qcom-iris-decoder`, Qualcomm's downstream iris driver (`drivers/media/platform/qcom/vcodec/iris`). It takes H.264, HEVC, and VP9 up to 8192x8192. AV1 isn't supported. The capture queue can produce `Q08C` (NV12 in Qualcomm's UBWC compressed layout), linear NV12, and NV21. It also lists RGBA (`AB24`, `QC24`), but those write a 10-bit UBWC YUV picture (S1), so the decoder has no usable RGB output.
- **10-bit works on Valve's kernel.** Steam Link VR's client (`vrlink.txt`, `SVLCodecV4L2`) decodes into `Q10C`, 10-bit UBWC, at 1152x4608. That is both eyes stacked in one frame, so it uses one decode session. Valve drives the decoder through the raw V4L2 stateful API, as `vr-recorder` does for the encoder.
- **Decoder budget.** The driver carries `MAX_SESSION_COUNT`, `MAX_MBPF`, and `MAX_MBPS` capability tables. For SM8650 the upstream values are 16 sessions, 278,528 macroblocks per frame, and 7,776,000 macroblocks per second (one 8K stream at 60 fps). A session that would go over the budget is refused when it starts. The downstream numbers on Valve's kernel are still to be confirmed by spike S2.
- **SteamVR imports YUV but doesn't convert it.** `IVRIPCResourceManagerClient::GetDmabufModifiers(VRApplication_Overlay, …)` returns LINEAR and `0x0500000000000001` (`DRM_FORMAT_MOD_QCOM_COMPRESSED`) for NV12, P010, and the RGB formats. The probe is in `~/.cache/remote-displays-spike/modprobe.cpp`. `DmabufAttributes_t` takes multiple planes. This is the call ft-screens already makes for KWin's buffers (`ft_vr_screen_present`, `vr.cpp:2080`). The import works, but vrcompositor samples the planes as they are: red shows Cr, green Y, blue Cb (S1). `DmabufAttributes_t` has no colour-space fields to change that.

So a decoded frame needs one GPU pass before SteamVR sees it: NV12 to RGBA, about 0.13 ms of GPU time for a 3440x1440 frame at full clock. It is the same kind of pass as the hand cutouts' (`screens/handcut.cpp`), and native screens pay for one of the same size: KWin's composite of each output.

## Is the decoder the bottleneck?

Mostly no. Macroblocks per second against the 7,776,000 budget, at 60 fps:

| Displays | Share of the decoder |
|---|---|
| 5x 1920x1080 | 31% |
| 5x 2560x1440 | 56% |
| The current layout (3440x1440 + 2x 1440x1920) | 32% |
| 5x 2560x1440 at 90 fps | 83% |
| 4x 3840x2160 | 100% |
| 5x 3840x2160 | 125%, refused |
| Steam Link VR's own stream (1152x4608 at 120 Hz) | about 32% |

Five 1440p displays fit at full rate with room left. Five 4K displays only fit at about 48 fps or less. That number is what gets declared at session start; actual load is lower, because hosts send frames only when the screen changes (see "Keeping the decoder under budget").

### The planned setup

Two hosts that share the same two desk monitors: a Mac (14-inch MacBook Pro) with those two plus its built-in display, and the test PC (Windows, RTX 5090) with the two. Windows and Linux hosts run Vibepollo, which streams real displays and creates virtual ones on demand. The Mac runs Sunshine and is limited to one display for now (see "Host side"). At 60 fps:

| Display | Share of the decoder |
|---|---|
| Super ultrawide 5120x1440 (5160x1440 as given; same load within 0.2%) | 22.2% |
| Built-in 3024x1964 (native pixels) | 17.9% |
| Ultrawide 3440x1440 | 14.9% |
| Mac, one display | 14.9% to 22.2% |
| The test PC, ultrawide + super ultrawide | 37.1% |
| Both hosts today (three displays) | at most 59.4% |
| Five: the three above plus two 3440x1440 virtual displays on the test PC | at most 89.2% |
| The original five (all three Mac displays + the test PC's two) | 92.2% |

Every combination fits under the driver's limit at 60 fps. The original five stay as the worst case for spike S2, so the Mac can grow past one display later without a new budget. These are declared numbers, with every display changing every frame at once. With damage-driven hosts the real load is far lower.

Notes:

- The 5120-wide display needs HEVC. H.264 hardware encoders stop at 4096 pixels wide, and the iris decoder takes HEVC up to 8192. HEVC for every stream keeps it simple.
- Streams don't need more than 60 fps. The Frame's display runs at 90 Hz, and the MacBook's 120 Hz ProMotion would double the built-in display's load for nothing.
- A stream can be smaller than its display, because the host scales before encoding. A panel in VR covers far fewer headset pixels than the display has; a 1 m wide panel at 1 m spans about 53°, which is roughly 1,000 headset pixels across (estimate; the Frame's pixels per degree haven't been measured here). The built-in display at 2268x1473 instead of 3024x1964 would cost 10% instead of 18%. That's the lever when Steam Link VR also needs the decoder, or when a virtual display is made larger.
- The desk monitors are shared through input switching. A monitor switched to the other machine may disappear from the first one, depending on the monitor and the cable. Real-display streams only work for monitors that host currently sees. In the headset, virtual displays avoid the question.

A remote display should cost less than a native one everywhere else:

| Per display | Native screen | Remote display |
|---|---|---|
| Apps | Run on the Frame's CPU | Run on the host |
| Composition | KWin draws each output on the Adreno GPU | One GPU pass per new frame turns the decoder's NV12 into RGBA |
| Hand-off to SteamVR | XRGB dmabuf, zero copy | RGBA UBWC dmabuf, zero copy |
| Sampled by vrcompositor | 4 bytes per pixel | The same |
| New work | — | Network receive and reassembly, V4L2 queueing |

The new costs are CPU for receiving packets, plus the decoder's and Wi-Fi radio's power and heat. Heat matters because the SoC slows down when it gets hot. Those are what the spikes need to measure.

## Keeping the decoder under budget

1. **Damage-driven hosts.** Sunshine and its forks send a new frame only when the captured screen changes, plus duplicates down to `minimum_fps_target` (default half the stream rate, settable to 1). With `minimum_fps_target = 1`, a static display costs about one decoded frame a second, the same idea as a native screen with no damage. A display playing video costs full rate, as a native video screen does.
2. **Out of sight means 1 frame per second, whatever the host sends.** A display outside your view updates once a second, even when the host is sending 60 fps from a game. See "Displays out of sight" below for how. Concealed panels, and every panel while the desktop is paused for a game, disconnect fully after a grace period. That frees the host's encoder and the network.
3. **An admission budget.** Before connecting, Frametop adds up the declared load of every stream: width/16 × height/16 × fps. If a new stream would go past the driver's limit, minus whatever else is decoding, Frametop lowers settings before connecting: first fps, then resolution, starting with the displays that get the least attention. Today's three displays come to at most 59%, which fits next to Steam Link VR's stream (about 32%). Five displays (89-92%) fit alone but not next to it, so a streamed PC game with five remote panels up would push them down to about 45 fps (or some lower and others higher).
4. **No B-frames, low-latency decode.** Sunshine doesn't send B-frames, so every decoded frame can be shown at once. The decoder gets a small capture pool (6-8 buffers). Valve's client found the standard `DISPLAY_DELAY` controls unsupported on this driver (`EINVAL` in `vrlink.txt`). They aren't needed: S1 showed that with `Q08C` each frame comes out as soon as it's decoded (linear NV12 holds two more).
5. **Tiling only as a fallback.** Valve tiles both eyes into one frame because they always change together. Desktops don't: one busy display would make the whole canvas decode at full rate, and a hidden tile can't be skipped. Per-display streams keep each display's cost proportional to its own changes. Tiling makes sense only for a host with many small, mostly static displays, and Sunshine can't capture a spanning desktop on Windows anyway.

The Moonlight protocol can't change resolution, fps, or bitrate mid-stream; that takes a reconnect. With stock hosts, attention changes therefore act on the client: in what gets decoded (2), not in what the host sends.

## Displays out of sight

Remote panels follow the native screens' attention rules (`UpdateAttention`, `vr.cpp:855`):

| Attention | Native screen | Remote display |
|---|---|---|
| Focused (within 12° of where you look) | Full rate | Full rate |
| In view (within 60°) | 15 Hz, or full rate while it plays video | Full rate. Every frame has to be decoded anyway (below), and the host sends only what changed |
| Hidden (out of view) | 1 Hz | 1 Hz, even when the host sends 60 |

The "video" signal comes for free: a damage-driven host sends frames only when its screen changes, so frames arriving faster than 10 a second for 8 frames in a row mean video, matching `VIDEO_COMMITS` and `VIDEO_HZ` in `compositor.c`.

The stream's fps is ours to choose. A game running at 144 Hz on the host is captured and sent at the fps the stream asked for (60 at most), so 144 never reaches the decoder.

**Why 1 Hz can't just skip frames.** Each frame in a stream is coded as changes to the frame before. Decoding frame 60 needs frames 1 to 59. Throwing away 59 of every 60 frames breaks the chain, and the next frame decodes as garbage. Valve's client works the same way: on a stall it asks the host for a new full frame (an "IFrame" in `vrlink.txt`).

**With stock hosts (Moonlight protocol): keyframe sampling.** While a panel is hidden, ft-stream:

1. drops every incoming frame before the decoder (moonlight-common-c's decode callback gets a `DECODE_UNIT` with `frameType`; return `DR_OK` without queueing);
2. calls `LiRequestIdrFrame()` once a second, and decodes and shows only the IDR frame that comes back, which needs no earlier frames;
3. when the panel comes back into view, requests one more IDR and goes back to decoding everything. The last frame, at most a second old, stays up until it arrives, about one round trip plus one frame later.

What that saves and what it doesn't, for a hidden display whose host sends 60 fps:

| Cost | Saved? |
|---|---|
| Decoder work | About 59 of 60 frames. One IDR costs a bit more to decode than one change frame |
| Panel updates in vrcompositor | 59 of 60 |
| Network traffic | No. The host keeps sending 60 fps, plus one larger IDR a second. A 5120x1440 IDR is a few hundred KB (estimate), so a few Mbit/s extra |
| Frame CPU to receive, reassemble, and repair packets | No. moonlight-common-c still handles every packet. Patching it (it's GPL, and so is ft-stream) to discard hidden video packets early would cut most of this |
| Host encoder | No |

**With a host that takes an fps change mid-stream: real 1 Hz.** If the host can be told "this display is out of sight, send 1 fps", the hidden display costs almost nothing anywhere. That's network, Frame CPU, decoder, and host encoder alike, and no IDRs are needed. The 1 Hz frames are ordinary change frames against the one a second earlier. This needs our own streamer, or a Sunshine fork with a control message that changes the capture rate. Owning a host streamer was rejected for its maintenance cost (see "Alternatives considered"), so this stays a possible upstream contribution. Vibepollo is the likeliest place to propose it: it already extends the protocol, since its own Moonlight fork sends a VRR pacing request that changes how the host captures.

Spike S3 checks the stock-host version: whether Sunshine honours an IDR request every second, how big and how late those IDRs are, and what receiving a hidden 60 fps stream costs the Frame's CPU.

## Proposed architecture

Frametop maintains only the headset side. The hosts run existing, separately maintained streamers that speak the Moonlight protocol: Vibepollo on Windows and Linux, Sunshine on macOS (see "Host side").

```
host machine: Vibepollo (Windows, Linux) / Sunshine (macOS), one stream per display
   │ RTSP + RTP over UDP, ENet control
   ▼
ft-stream (one process per remote display, on the Frame, GPLv3)
   moonlight-common-c (protocol) + pairing (moonlight-embedded's libgamestream)
   iris decoder session: OUTPUT = HEVC, CAPTURE = Q08C, VIDIOC_EXPBUF
   GLES pass: Q08C → ring of 3 RGBA buffers (EGL YUV hints = the stream's colour space)
   │ unix socket, SCM_RIGHTS
   ▼
ft-screens (existing, MIT)
   remote screen type: ImportDmabuf once per ring buffer,
   SetOverlayTexture per frame, buffer returned to ft-stream when replaced
   panel input → ft-stream → LiSendMousePositionEvent / LiSendKeyboardEvent / …
```

**Why one process per display.** moonlight-common-c keeps global state, so it runs one stream per process. It and libgamestream are GPLv3 while Frametop is MIT, and a helper that talks over a socket keeps the licences apart (this isn't legal advice). ft-stream lives in its own directory with its own licence file. Separate processes also mean a stalled stream or a decoder reset can't freeze the native screens. Each process pairs as its own client, which Vibepollo needs anyway: it ties each Remote Monitor to the client that opened it, one role per client, so every display needs its own client certificate and pairing.

**Pairing with a host token (chosen 2026-10-07).** Since every display is its own client, PIN pairing would mean one PIN and one round of permission clicks per display. Vibepollo has neither a multi-use PIN (its one-time PINs pair one client each) nor a way to pair several certificates at once. It does have scoped API tokens for its Web UI API (`POST /api/token`, sent as `Authorization: Bearer`). So the user creates one token on the host, limited to submitting pairing PINs (`POST /api/pin`), listing clients and setting their permissions (`GET /api/clients/list`, `POST /api/clients/update`), listing the host's monitors (`GET /api/display-devices`), and placing Remote Monitors (`GET`/`PUT /api/clients/display-layout`). A script on the host makes it from the Web UI login (`make-frametop-token`), and the user enters it once in Frametop's "add host" dialog. From then on, Frametop makes a client for each new display, pairs it by sending its own PIN, and grants it launch, mouse, and keyboard (Vibepollo gives a new client only list and view). It sizes Remote Monitors like the host's real monitors, and unpairs a display's client with that client's own certificate when the display is removed. The token can't change the host's settings and can be revoked in the Web UI. It can change any client's permissions, though, so Frametop keeps it like a password (a file only the user can read). Vibepollo's client update replaces the whole client record, so Frametop always sends every field.

**Why ft-stream converts.** ft-screens then gets RGBA dmabufs, as it does from KWin. Each decoder buffer goes back to the decoder right after the pass, so the decoder's pool doesn't depend on what SteamVR still holds. A GPU fault in one stream stays in its own process. ft-stream asks the host for BT.709 limited range (Moonlight's colour space and range settings) and gives the converter the same as EGL hints. S1 showed all four matrix and range combinations come out right when the hints match the stream.

**Opening a display.** For a host's main session, ft-stream launches the host's desktop app as Moonlight does. For a Vibepollo virtual display it launches the synthetic "Remote Monitor" app (id `2147483505` in `remote_session.h`), and the requested stream size and fps become the virtual display's mode. When the stream drops, Vibepollo keeps that display and its windows by default (`remote_monitor_disconnect_on_stream_end = false`), and ft-stream reconnects with "Resume" (id `2147483501`). So a concealed panel can disconnect fully without the host rearranging its windows.

**The socket protocol**, roughly:

- ft-stream → ft-screens: `buffers` (count, width, height, DRM format, modifier, offset and pitch, with the dmabuf fds of the RGBA ring) after each (re)configuration; `frame i` when ring buffer i holds a new picture (sent once the GPU is done, so no fence is needed); `title`, `state` (connecting, live, waiting for a keyframe, lost).
- ft-screens → ft-stream: `release i` when buffer i is no longer on screen; `attention focused|view|hidden|concealed`; input events.

**In ft-screens**, the explorer's map gives the seams:

- Remote screens get `g_screens` entries through a `MakePanel` variant, with indices out of KWin's first-free-slot range (`compositor.c:279`) and the overlay keys `frametop.remote.N`. That brings visibility, attention, lasers, controls, spin, pinning, and hand cutouts.
- Frames enter through an eventfd on the wl event loop and go through the same `ft_vr_screen_present` path, keyed by ring buffer, so the `g_imports` cache hits: 3 imports per display.
- The pointer helper needs the new prefix in `FramePanel` (`ft-pointer.cpp:485`). The `screens` reply and `get N` need to list remote screens so gaze hit-testing sees them (`ft-gaze.cpp:302`).
- `frametop-layout.json` gets a separate `hosts` list, since the `screens` array also sets KWin's output count. The user adds a host once, and its displays come as a bundle: Frametop pairs a client per display, starts their streams when the desktop starts, and stops them with it. Each host entry has the address, the token's file, and its displays. Each display has which one it is (the main session or a Remote Monitor), its own stream settings (size, fps, bitrate, and later codec and HDR), and the usual place, width, curve, and pin. ft-layout, profiles, and Display Settings learn the new list; Display Settings shows a host with its displays under it, each with its own settings (user decision, 2026-10-07).

**Input.** `handle_vr_event` (`compositor.c:333`) branches on the screen type. A remote screen sends:

- pointer motion as `LiSendMousePositionEvent(x, y, w, h)` in stream pixels;
- buttons as `LiSendMouseButtonEvent`;
- scroll as `LiSendHighResScrollEvent`, which matches the existing notches × 120.

A drag can cross panels, for example a window dragged from one of the host's displays to another. SteamVR sends a held button's moves only to the panel where the press began, with coordinates off that panel once the laser leaves it. The panel under the laser gets nothing (S3 measured this). So while a button is held on a remote screen, ft-screens hit-tests the laser (`ComputeOverlayIntersection`) against the host's other remote screens and sends the position to the ft-stream of the screen it hits. Moves that land on no panel are dropped, never clamped: clamping pins the host's cursor to the first display's edge. The host's input is shared across its sessions, so Windows sees one drag. But the release goes through the stream the press went through: Vibepollo takes a mouse button's release only from the client that pressed it (`mouse_press_owner` in its `src/input.cpp`). Sent through the display under the laser, it was dropped, and the window stayed on the pointer (found in the headset, 2026-10-07; `g_pressed_on` in `remote.c`).

Keyboard focus follows the last panel clicked. When it's a remote one, `send_key` and `relay_button` send evdev codes to its ft-stream, which maps them to Windows virtual-key codes (moonlight-qt has the table). Sunshine on macOS maps the Windows key to Cmd and Alt to Option. The host draws its own cursor into the video, so it lags the laser by one round trip.

**Won't work across the boundary:** drag and drop, the clipboard (the Moonlight protocol has none), floating windows, and KWin window rules. A remote display is a picture of another machine's monitor.

## Host side

**Decision for v1 (2026-10-07): real monitors only.** Vibepollo 2.0.0's Remote Monitors broke the test PC's monitor layout every time one was removed (see "S3 results so far"), while streaming a real monitor changes nothing on the host. Frametop can make extra screens in the headset itself, and the feature is mostly for controlling other machines, so v1 streams only a host's real monitors; virtual displays wait until the Vibepollo bugs are fixed. One host program streams one real monitor (a second client launching the main app joins the display already streaming), so each extra real monitor needs its own host instance: its own config file and ports (`port` 100 apart) and `output_name` set to that monitor. On the test PC that's Vibepollo for the primary (it also serves the Steam Deck and the Mac) and a plain Sunshine instance for the LC34G55T. Plain Sunshine has no scoped API tokens, so a Sunshine instance is paired once by PIN; it has a single Frametop client anyway. Frametop's host bundle becomes a list of instances, one per real monitor.

The requirement: each host streams its real displays, and virtual displays it creates on demand. Frametop doesn't maintain a host streamer; it uses existing ones and ships setup notes or scripts for them. State checked 2026-10-06.

| Host OS | Streamer | Displays |
|---|---|---|
| Windows | Vibepollo | 1 main session (real or virtual) + up to 4 virtual Remote Monitors |
| Linux (Arch, CachyOS; beta) | Vibepollo | The same, with at most 4 virtual displays at once |
| macOS | Sunshine | 1, the Mac's main display, for now |

### Windows and Linux: Vibepollo

Vibepollo 2.0.0 (2026-09-30) is a Sunshine fork. One install gives:

- **A main session.** As on any Sunshine host, this is a real display (`virtual_display_mode = disabled`, picked by `output_name`) or a virtual display created for the client (`per_client`, the default on Windows 11 and Linux).
- **Up to four Remote Monitors** (`max_client_vdds = 4` in `remote_session.h`). Each is a virtual display at the size and rate its client asks for, streamed and captured on its own (`capture_plan` in `remote_session.cpp`). They don't need a main session: with nothing running, the app list still offers Remote Monitor. They are always virtual and can't stream a real display.

That covers virtual displays completely, up to five from one host. Real displays are the limit: only the main session shows one, so one Vibepollo install streams one real display. A second real display at the same time needs a second host instance with its own config file, port, and `output_name`. On Windows that could be a plain Sunshine instance next to Vibepollo (untested). On Linux, Vibepollo expects to be the only host install on the machine. For the test PC this means one desk monitor streams as the main session and further displays are virtual, unless S3 shows a second instance works. Virtual displays also avoid the input-switching question (see "The planned setup").

Settings Frametop's setup notes change from the defaults:

- `virtual_display_layout`: the default `exclusive` turns off the host's other monitors while a virtual main session streams. `extended` keeps them on; `extended_isolated` also stops the host's own mouse from wandering onto the virtual display. Remote Monitors don't use it: Vibepollo always adds them next to the monitors already on (`apply_remote_monitor_composition` in `nvhttp.cpp`).
- `minimum_fps_target = 1`, so a static display costs about one frame a second.
- `remote_monitor_mute_audio = true`, unless that display should carry audio.

Both platforms send frames only when the screen changes. On Linux the virtual displays use presentation-driven capture: sparse changes are captured at once, and faster ones are coalesced to the stream's fps. The virtual outputs have no cursor plane, so the cursor is in the video, as with every Moonlight host.

PyroWave, Vibepollo's wavelet codec, isn't usable here. It decodes on the client's GPU and needs hundreds of Mbit/s over wired LAN. Frametop uses HEVC.

**Windows (the test PC).** The chosen setup (2026-10-06): one desk monitor streams as the main session, a real display (`virtual_display_mode = disabled`, `output_name` set to it). The other desk monitor is replaced by a Remote Monitor, a virtual display at that monitor's size, which Vibepollo releases when Frametop ends the connection (`remote_monitor_disconnect_on_client_disconnect = true`). A dropped connection keeps it (`remote_monitor_disconnect_on_stream_end = false`), so a Wi-Fi drop or a panel that disconnects while out of sight doesn't move its windows; ft-stream releases it explicitly with "Disconnect Monitor" (id `2147483502`) when the remote display is removed or Frametop exits. Each Remote Monitor's place next to the real monitors is set per client in the Web UI. GeForce allows 8 concurrent NVENC sessions per system. Installing Vibepollo and its display driver needs an admin; the test PC's `maptrainer` account isn't one.

How it's set up on the test PC (2026-10-06). Vibepollo installed over Apollo in `C:\Program Files\Apollo` (service `ApolloService`) and kept Apollo's settings, pairings, and Web UI login. The existing clients (a Steam Deck and a Mac) launch "Desktop" or "Steam Big Picture", which stream a virtual display with the other monitors turned off (`dd_configuration_option = ensure_only_display`). Frametop leaves them alone and gets its own app instead:

- "Frametop primary display": `"display-output": ""` streams the real primary monitor, and `"dd-configuration-option": "disabled"` leaves the other monitors as they are. ft-stream launches this app for the main session. These are the fields the Web UI writes for "use my own display" (`process.cpp` reads them).
- `minimum_fps_target = 1` globally, which Remote Monitors use. "Desktop" and "Steam Big Picture" keep 120 as per-app `config-overrides`, so the Deck and the Mac stream as before.
- `remote_monitor_mute_audio`, `remote_monitor_disconnect_on_client_disconnect` on, `remote_monitor_disconnect_on_stream_end` off.

The config folder is admin-only, so the change is a script for the user to run (`D:\remote-displays\vibepollo-setup\run-setup.cmd`, with a backup and an optional Web UI password reset). It ran on 2026-10-07. Vibepollo rewrites apps.json each time it starts and adds its built-in "Remote Input" and "Remote Monitor" apps, so any later change must start from the live file.

**Linux (beta).** x86_64 only: Arch Linux or CachyOS, KDE Plasma 6 on Wayland started by SDDM or Plasma Login Manager, Linux 6.16 or newer with matching headers, and a GPU with hardware H.264 encode (tested on NVIDIA and modern AMD). Virtual displays come from Vibepollo's own DKMS module, `vibeshine_drm`, which has four connectors, so at most four virtual displays exist at once. Streaming before login needs NVIDIA. Other distributions aren't covered; plain Sunshine still streams real displays there, without virtual ones.

**Risk.** Vibepollo is new, moves fast, and has one maintainer, and its README says about 99% of its code is AI-generated. Frametop depends only on its Moonlight-protocol behaviour and the Remote Monitor app ids, so on Windows Apollo (virtual displays through the SudoVDA driver) or plain Sunshine (real displays) stay as fallbacks.

### macOS: Sunshine, one display for now

Vibepollo has no macOS build. Sunshine (v2026.914, labelled experimental on macOS) is the only Moonlight-protocol host for the Mac, so the Mac streams one display: its main display, from one Sunshine instance. The reasons:

- Since v2026.906, absolute mouse input only reaches the main display (Sunshine #5733). Any other display would show but couldn't be clicked.
- Nobody reports running several Sunshine instances on one Mac yet.
- Sunshine can't create virtual displays on macOS.

The main display is the one with the menu bar (System Settings → Displays). Capture is at backing pixel size, so the built-in display streams at 3024x1964. Closing the lid removes the built-in display, so a stream of it ends. Capture stalls if the display sleeps mid-stream (#5509).

One display can still reach the encoder's limit. Sunshine forces VideoToolbox's low-latency mode, which roughly halves throughput (#5814); an M4 Pro managed 0.36-0.46 Gpix/s in that mode. The super ultrawide at 60 fps is 0.44 Gpix/s, so full-motion video on it may need a lower fps or a smaller stream. The other two displays need 0.30 and 0.36 Gpix/s.

**What would lift the limit.** The mouse fix is libvirtualhid PR #145 ("target configured mouse viewport", open), which Sunshine's draft PR #5739 pulls in. Once it ships, spike S4 tries one Sunshine instance per display (own config file, `port` about 100 apart, `output_name`) and BetterDisplay virtual displays switched on by `global_prep_cmd`. Open PR #5817 (ScreenCaptureKit + OBS's VideoToolbox encoder) may raise the encoder limit. Helping #145 along upstream is the one Mac contribution worth making.

### What stock hosts cost us

| Limit | Effect | What can be done |
|---|---|---|
| A stream's fps is fixed at connect | Out-of-sight panels save decoder work through keyframe sampling, but the host keeps encoding and sending 60 fps | Patch ft-stream's copy of moonlight-common-c to drop hidden video packets early (saves Frame CPU). A "change rate" control message needs a host upstream; Vibepollo is the likeliest |
| One stream per display | Five ft-stream processes, connections, and client pairings on the Frame | Measure the CPU (S3) |
| One real display per Vibepollo install | A second real display needs a second host instance | Use virtual displays; S3 tries a second instance on Windows |
| Cursor drawn into the video | Lags the laser by a round trip; each mouse move over a still page becomes a video frame | Accept |
| Mac: one display | Sunshine's mouse reaches only the main display, and there are no virtual displays | Upstream fix in progress, then S4 |
| Mac encoder | Full-motion video on the super ultrawide at 60 fps is at the limit | Lower fps or size for that stream |

What Frametop maintains: ft-stream (pairing, decoder, keyframe sampling, input mapping, Remote Monitor launch and resume; the protocol itself is moonlight-common-c's), the remote screen type in ft-screens, the layout and settings changes, and host setup notes or scripts.

## Alternatives considered

- **Our own host streamer (ft-host)** on macOS, Windows, and Linux: capture and encode each display at a rate the headset can change mid-stream, real 1 Hz for hidden panels, a separate cursor, one connection per host, virtual displays built in. It solves every limit in the table above, but it means maintaining capture, encoding, transport, input, pairing, and virtual displays on three operating systems. Rejected for a free project (2026-10-06).
- **Moonlight-qt in a window on a Frametop screen.** Nothing to build, and a quick way to check that a host and pairing work. But each frame goes decoder → Moonlight's GL renderer → KWin → ft-screens: two GPU passes per display where ft-stream needs one. Hidden panels can't drop to 1 Hz, and FFmpeg's v4l2m2m path on this device hasn't been tried (its encoder segfaults). Useful as a baseline, not as the feature.
- **RDP (FreeRDP client, RDPGFX).** The best multi-monitor design on paper: one session, up to 16 monitors as separate surfaces, damage rectangles. But Windows' RDP host takes over the login session (the PC's own monitors lock) and runs at 30 fps by default, macOS has no RDP host, and FreeRDP decodes H.264 on the CPU or through VA-API, which the Frame lacks.
- **Parsec, Steam Remote Play, RustDesk, NoMachine, Selkies, Apple Screen Sharing.** No aarch64 Linux client with hardware decode, no per-monitor streams, a closed protocol, or a Mac-only client.

## Spikes before any Frametop change

Spikes on the Frame run through `frame-job --local` as a standalone test overlay, with the headset on or with frame-testbench holding its worn state and pose. None of them touches the live desktop. S3 and S4 also need the hosts set up.

| # | Question | Pass |
|---|---|---|
| S1 (done) | Does a decoded `Q08C` buffer show in a SteamVR overlay with correct colours? Test pattern clip, BT.709 limited range, then full range | Correct colours; under 3% of a core; no extra missed frames (`~/.cache/frametop-perf/drops`); the decoder's real output delay |
| S2 | Concurrency: the original five, the worst case (2x 5120x1440, 2x 3440x1440, 3024x1964), fed from files in real time at 60 fps | Admitted (92% declared); decode time per frame; SoC temperature and clocks over 10 minutes |
| S3 | Live streams from the test PC (Vibepollo): the main session on a real desk monitor plus two Remote Monitors, through moonlight-common-c + the S1 decoder; then keyframe sampling on a hidden 60 fps stream | CPU per stream at real bitrates; glass-to-glass latency; time to picture after `LiRequestIdrFrame()`; the host honours one IDR request a second; IDR size; Frame CPU for a hidden stream; each Remote Monitor's mouse lands on its own display; Resume after a dropped stream keeps the windows; whether a second host instance can stream the other real monitor |
| S4 | The Mac: one Sunshine instance on the main display. Once the mouse fix ships: one instance per display, then a BetterDisplay virtual display | Mouse and keyboard work; encode rate with full-motion video on the super ultrawide; `minimum_fps_target = 1` keeps a static display near 1 fps; later, all three stream at once with the mouse on the right display |
| S5 | The baseline: 5 native screens, one playing video, four static | The CPU, GPU time (DRM fdinfo), temperature, and missed-frame numbers that the remote version must match |

### S1 results (2026-10-06)

S1 passes, with one change to the plan: a GPU pass between the decoder and SteamVR.

`stream/spike/ft-dectest` (built by `stream/build.sh`) decodes a clip with the V4L2 stateful API and hands each picture to SteamVR with `ImportDmabuf`, as the remote screen type would. Clips: a colour test pattern (`stream/spike/pattern.py`) with a moving box, 10 s at 60 fps, encoded by NVENC HEVC on the test PC (`-preset p1 -tune ull -bf 0`, as Sunshine-style streaming), in BT.709 and BT.601, limited and full range. The in-headset runs used frame-testbench to hold the worn state and a still pose, with nobody wearing the headset.

**Decoding.**

| | 3440x1440, Q08C | 3024x1964, Q08C | 3440x1440, linear NV12 |
|---|---|---|---|
| Decoder's buffer (coded size) | 3456x1440, 7,557,120 bytes | 3072x1984, 9,191,424 bytes | 3456x1440, 7,467,008 bytes |
| SteamVR import | Works (NV12 + `QCOM_COMPRESSED`) | Works | Works (NV12, linear) |
| Decode time, steady | 2.1 ms avg, 2.7 ms worst | 2.6 ms avg, 3.3 ms worst | 2.2 ms avg, 2.8 ms worst |
| First picture after | 1 input frame | 1 input frame | 3 input frames |
| CPU at 60 fps, no conversion | 1.2% of a core | 1.5% of a core | 1.4% of a core |

- The buffer layout from `msm_media_info.h` (per plane: UBWC metadata, then pixels, each 4 KiB aligned; Y stride aligned to 128, heights to 32) matches the driver's size to the byte, so the import offsets are right. The visible size comes from `G_SELECTION`; the decoder pads the coded size.
- Q08C has no extra delay: each frame comes out as soon as it's decoded. Linear NV12 holds two more frames, so the remote screen type uses Q08C.
- The CPU figure is ft-dectest's own process (file reading, V4L2 queueing, the overlay call). Network receive is S3's.

**Colours.** `stream/spike/s1-colours.py` shows the clip head-locked, switches the panel between the decoded video and an RGB copy of the pattern every 2.5 s, grabs the headset view (`/dev/video99`) in both states, and compares each colour patch.

- Decoder buffers straight into SteamVR come out wrong. vrcompositor doesn't convert NV12: red carries Cr, green Y, blue Cb. 100% red (Y 63, Cb 102, Cr 240 in BT.709 limited range) showed as 239/62/102, and greys as a dull magenta.
- The decoder's RGBA formats don't help. It accepts `AB24` and `QC24` but writes a 10-bit UBWC YUV picture into them: 10,027,008 bytes used, exactly TP10 UBWC at 3456x1440, starting with UBWC metadata. It also reports their `bytesperline` in pixels.
- A GPU pass gets them right (`ft-dectest --convert 709|601 --range tv|pc`). GLES samples the decoder's buffer through an external texture with the matrix and range as EGL hints (`EGL_YUV_COLOR_SPACE_HINT_EXT`, `EGL_SAMPLE_RANGE_HINT_EXT`). It draws into a ring of 3 RGBA buffers (UBWC, `QCOM_COMPRESSED`) that SteamVR imported once, set up as in `screens/handcut.cpp`.

| Clip, converted with its own matrix and range | Mean error | Worst patch |
|---|---|---|
| 3440x1440, BT.709 limited | 1.4 | 4.0 |
| 3440x1440, BT.709 full | 1.2 | 4.0 |
| 3440x1440, BT.601 limited | 0.2 | 2.4 |
| 3440x1440, BT.601 full | 0.3 | 2.9 |
| 3024x1964, BT.709 limited | 1.3 | 4.0 |

Errors are on the 0-255 scale, over 39-40 patches (27 for 3024x1964, where less of the panel is in view). The near-black steps (0-30) and near-white steps (225-255) all stay apart, so nothing is crushed or clipped. BT.709 sits about 3 low in red throughout, probably a rounding difference between ffmpeg's and Mesa's coefficients. That isn't visible.

**Cost of the pass**, 3440x1440 at 60 fps:

| Measure | Result |
|---|---|
| GPU work | About 118,000 cycles a frame: 0.13 ms at the 903 MHz top clock. With the headset idle, the GPU ran at about 230 MHz and was busy 0.51 ms a frame (DRM fdinfo) |
| Time until the GPU is done | 1.1-1.4 ms avg, 2-6 ms worst |
| CPU, whole ft-dectest process | 2.2-3.5% of a core, up from 1.2% |
| Missed compositor frames, 30 s at 90 Hz | 0 of 2,701 with the stream shown, 0 of 2,700 idle |

- The CPU is at the 3% pass line. The increase is the GL driver plus the `glFinish` wait; ft-stream should try waiting on a sync file in its poll loop instead.
- The five-display worst case (89% of the decoder) is about 360 converted 3440x1440-sized frames a second. That's about 5% of the GPU at top clock, and S2 measures it.
- frame-testbench held the pose still and nobody wore the headset, so tracking and eye-tracking load may differ from a real session. S5 rechecks missed frames with the headset worn.

The benchmark is S5 against the same scene on remote displays: total Frame CPU (Frametop + ft-stream), GPU time, missed frames, and SoC temperature all at or below native.

### S3 results so far (2026-10-07)

`stream/spike/ft-streamtest.cpp` is a Moonlight client for one display: moonlight-common-c and libgamestream from moonlight-embedded (pinned in `stream/build.sh`), S1's decoder and GPU pass (`spike/iris.h`), and a SteamVR overlay. It pairs through the host's API token, launches the primary app or a Remote Monitor, and reports every 2 s. Tests ran from the test PC over the LAN, with the headset held by frame-testbench.

**The primary display (5120x1440 at 60 fps, HEVC, 50 Mbit/s asked).** It works end to end. The picture in the headset is right, colours included. The test PC's primary is an HDR monitor, and Vibepollo sends it as SDR Rec. 709, as asked.

| | Shown | Hidden (keyframe sampling) |
|---|---|---|
| Frames received | 60 fps | 60 fps |
| Frames decoded and shown | 60 fps | 1 fps |
| Network | 12-14 Mbit/s | the same |
| ft-streamtest CPU | 4.2-4.6% of a core | 1.5-1.7% of a core |

- Time from a frame's first packet to the panel: 5.1-5.4 ms on average (worst about 12 ms). Of that, decoding takes 3.7 ms, and receiving the whole frame 0.1 ms. The host reports 3.2 ms from capture to encoded, and half the round trip is 1.5-2.5 ms. So a frame reaches the panel about 10 ms after the host captures it, before vrcompositor shows it. Glass to glass isn't measured yet.
- Connecting took 225 ms, and the first picture showed 0.5 s after the launch.
- Keyframe sampling: the host answered all 14 IDR requests, one a second. Each IDR arrived 18 ms after the request (worst 25 ms), reached the panel at 25 ms (worst 32 ms), and was about 81 KB for this desktop. Going back to full rate took 17 ms.
- No packets were lost.
- The test PC's desktop has an animated wallpaper, so the stream ran at 60 fps throughout, though Vibepollo applied a 0.5 fps floor. A still wallpaper would let an idle display drop to about one frame a second.

**Vibepollo bug: a stale display slot blocks Remote Monitors.** The first Remote Monitor launch failed with 503, "The composed display topology did not apply", for 40 s of retries. Vibepollo still held a display slot for the Mac's earlier "Desktop" session (`/api/clients/display-layout`: the Mac as a client node, its runtime "retryable" with the lease held), though that session's virtual display was gone. Composing the layout includes every held slot, and the Mac's can't be resolved to a device, so every composition fails. The Mac's session had been cut by a Vibepollo restart and reconnected before it ended. The failed launch still created frametop-2's virtual display, and "Disconnect Monitor" then reported success without removing it, because the launch never finished. A Vibepollo restart clears both. ft-stream must detect this state, a 503 that persists, and report it rather than retry forever. It's worth reporting upstream with the steps that cause it.

**Remote Monitor (frametop-2, 3440x1440 at 60 fps), after a Vibepollo restart.** The launch took 2.3 s (Vibepollo makes the virtual display, then answers), and the first picture came 0.9 s after connecting. The picture was right. An empty, still monitor dropped to 1 fps at once: 0.1 Mbit/s, 0.6% of a core, about 4-5 ms from first packet to panel. Windows moved a window it remembered for that spot onto the new monitor, so a Remote Monitor can take windows off the real screens without being asked.

**Vibepollo bug: a dropped Remote Monitor reset the host's real monitor layout.** The test killed ft-streamtest to imitate a dropped connection. Vibepollo kept the Remote Monitor for Resume, as configured, and recomposed the display layout. Windows refused it (`SetDisplayConfig`: ERROR_INVALID_PARAMETER, "failed to move device ... to new origin" for the monitor above the primary). Vibepollo's recovery (`SDC_USE_DATABASE_CURRENT`, a topology jog, `CDS_RESET`) then left Windows with a different layout: the monitor above became the primary, beside the old primary. "Disconnect Monitor" again reported success and did nothing. A Vibepollo restart was needed again.

One more defect that may have contributed: `GET /api/clients/display-layout` rebuilds Vibepollo's record of the real monitors without their positions or modes (every monitor at 0,0, 1920x1080; `refresh_remote_display_physical_baseline` in `confighttp.cpp`). A layout composed from that record stacks the monitors on one another. Launching a Remote Monitor reads the real positions again (`refresh_remote_monitor_baseline` in `nvhttp.cpp`), and the last such call came before the launch here, so this isn't proven to be the cause. Frametop must not call that endpoint until it's fixed.

So far, Vibepollo 2.0.0's Remote Monitors aren't reliable on the test PC: a stale slot blocks them, a drop can rearrange the host's real monitors, and "Disconnect Monitor" can't release a monitor whose ownership was lost. These go upstream with logs before Frametop depends on them. Further Remote Monitor tests on the test PC need the user's go-ahead, since they can move the user's own screens.

**The cause, and a fix (frametop-vibepollo, 2026-10-07).** All three Remote Monitor failures come from one call. When a display role ends, Vibepollo's coordinator (`src/remote_display_topology.cpp`) first recomposes the topology without the departing display, and only then removes it. On the test PC, Windows refuses that composed `SetDisplayConfig`. Then:
- libdisplaydevice's automatic recovery (`SDC_USE_DATABASE_CURRENT`, a topology jog, `CDS_RESET`) rearranges the real monitors;
- the release rolls back, so the virtual display stays attached and "Disconnect Monitor" does nothing;
- for a normal game (the Mac's session), the per-client identity stays held, and every later composition fails on it.

Removing the virtual display alone, as a Vibepollo restart does, left the layout intact. The fork [DeeJanuz/frametop-vibepollo](https://github.com/DeeJanuz/frametop-vibepollo) (GPL-3.0, like Vibepollo), on branch `fix/windows-remote-monitor-release`, changes two things on Windows:
- The coordinator removes the departing display first and recomposes only if another owned display remains (`retire_before_recompose`, set by the Windows runtime). A normal game's identity is released even when its display is already gone. Linux keeps the old order, which exists so KWin never has zero outputs.
- Composed layout applies run with display recovery off, so a refused layout can't reset the host's arrangement.

Four new unit tests cover it, and the coordinator's 40 tests pass. The plan is to offer the fix upstream once it's proven on the test PC. Frametop stays MIT: it only talks to the host over the network.

**The fix works on the test PC (2026-10-07).** The dev build, `2.0.0` plus the fix, was built on the test PC with MSYS2 (UCRT64, as the CI does, without WebRTC, drivers or packaging; `D:\vp-build`). It links only Windows system DLLs, so it drops into the install as a plain exe swap (`D:\vp-build\deploy.ps1`; the original is kept as `sunshine.exe.2.0.0-original`, and `-Restore` puts it back). Three Remote Monitor cycles left the real layout exactly as it was, with no layout re-apply and no `SetDisplayConfig` errors in Vibepollo's log: a clean end, a killed client, and a relaunch after the kill. Before the fix, the clean end reset the layout twice in a row. Restarting Vibepollo for the deploy also ended a leftover Mac session cleanly and brought the real monitors back.

A dropped connection removes the Remote Monitor at once (Vibepollo saw the disconnect within 3 s), even with `remote_monitor_disconnect_on_stream_end = disabled`. With `remote_monitor_disconnect_on_client_disconnect = enabled`, a lost connection counts as a client disconnect. Keeping a monitor and its windows across a Wi-Fi drop would need that setting off, and Frametop releasing monitors explicitly with "Disconnect Monitor". That's still to test.

The development loop is an incremental Ninja build on the test PC (only changed files recompile), the exe swap through the admin SSH login, and `ft-streamtest`: a few minutes per change. Pure logic gets unit tests on the Frame.

**Real displays, several at once (frametop-vibepollo, 2026-10-07).** A new hidden control, "Frametop display" (id 2147483521), streams one of the host's existing displays, named by the launch argument `frametopDisplay` (a device id from `/api/display-devices`). The session captures that output exactly, the way a Remote Monitor captures its virtual display, but nothing is created and the layout is never touched. Each client holds one such stream, and the main app stays free for the Deck and the Mac. On the test PC:
- both real monitors streamed at once (OLED at 5120x1440, LC34G55T at 3440x1440), each to its own client, at about 0.6% of a core each while idle;
- a real display and a Remote Monitor streamed side by side, and the layout was unchanged afterwards.

With that, Frametop no longer needs the "Frametop primary display" app; every real display is captured the same way.

**Vibepollo bug: one idle HTTPS client froze the API for everyone.** While any stream ran, every other client's HTTPS requests (serverinfo, pairing, launch) waited until it ended, so a second display couldn't start. The stock 2.0.0 build did the same. A stack dump (MSYS2 gdb attached to the service) showed the HTTPS server's only I/O thread blocked in a synchronous TLS shutdown: `SunshineHTTPS`'s destructor (`nvhttp.h`) sends close_notify and then waits for the client's. libgamestream leaves its connection idle in curl's cache (it forbids reuse only on FreeBSD), so the wait lasted as long as the streaming client process. Fixed on both sides:
- the fork marks the client's close_notify as received before shutting down, so nothing waits (a deliberately idle client no longer delays another client's request: 0.13 s);
- `ft-streamtest` drops libgamestream's curl handle after every request, which ft-stream must do too.

**The 3D mouse, and dragging windows between displays (2026-10-07).** The setup had three panels from the test PC: the OLED and the LC34G55T as Frametop displays, and a 2560x1440 Remote Monitor. frame-testbench held the headset, and the 3D mouse was driven through `@ft_pointer_helper`. The 3D mouse moved and clicked the test PC's cursor on every panel.

The first version clamped every move to its own panel. A drag from the Remote Monitor to the OLED left the window below the Remote Monitor's bottom edge, mostly off screen. Removing the Remote Monitor brought it back to the OLED. Drags between monitors that touch in Windows' layout seemed to work, but only because the cursor, pinned at the shared edge, left the window hanging across it. `--input-log` showed why. With the button held, SteamVR kept sending the moves to the panel where the press began (for example -213,2093 on the 2560x1440 panel), and sent the panel under the laser no events at all. The release also went to the first panel, off its edge.

The spike's fix is the routing described under "Input", with one difference: each panel is its own process. The panel that got the press publishes the laser's device in a small shared file (`/dev/shm/frametop-streamtest-drag`). Every other panel hit-tests that device's ray against itself and moves the host's cursor while the ray hits it. When hovering, the panel's own hit test matched SteamVR's coordinates exactly, so the ray origin and the UV orientation (bottom-left origin, like the mouse events) are right. With the fix, three drags worked:
- Remote Monitor to OLED;
- OLED to Remote Monitor;
- Remote Monitor to LC34G55T.

In each, the window followed the laser across panels and stayed where it was dropped. The release still went through the first panel's connection and landed right every time.

Windows' "Remember window locations based on monitor connection" moves windows on its own. When a Remote Monitor appears, Windows puts back windows it remembers there, and when it goes, they move to a real monitor.

## In ft-screens (2026-10-07)

The user decided remote displays are Frametop displays, with the same controls as the desktop's screens and their places saved in profiles. The spike's standalone overlays are replaced.

**What's built (branch `remote-displays`, uncommitted):**
- `stream/ft-stream.cpp` is the per-display helper. It pairs, launches and decodes, and converts into a ring of three RGBA buffers. It hands their dmabufs to ft-screens once, then says which buffer holds each new picture. The protocol is in its header comment: frames one way, then release, attention, pointer, keys and blur the other way. `stream/host.cpp` holds the pairing and launch code ft-streamtest had.
- `screens/remote.c` starts one ft-stream per remote screen with a socket pair, restarts a stream that ends (2 s, then backing off to 30 s), shows its frames through `ft_vr_screen_present`, and sends it the panel's input and attention. Commands: `remote <N> start <client> <host> <app> <W>x<H> <fps> <kbit/s> <metres> [label]` (`ok restored` when its panel is back where it was, below), `remote <N> stop`, `remote <N> info` (the stream's whole state, such as `lost can't connect`) and `remotes`. Remote screens are numbered from 101, so every other command (`place`, `width`, `curve`, `pin`, `get`, `conceal`) works on them as it is. `screens` leaves them out, because ft-layout, Display Settings and ft-floatd count KWin's outputs with it.
- `vr.cpp` gives a remote screen a screen's panel (`frametop.remote.N`) with its controls. A press on a remote screen dragged onto another one is routed there (`UpdateRemoteDrag`). SteamVR keeps sending the moves to the panel the press began on, as if its surface went on past its edges. So every tick the pressing laser is hit-tested against all the remote screens, and the nearest one it meets takes the moves. On the panel the press began on, SteamVR's own moves count only while that panel is the nearest. Before this (2026-10-07), that panel's carried-on surface passed in front of or behind the other panel, and its moves pulled the host's pointer back: drags across worked only some of the time. The release comes up where the host's pointer is, through the stream the press went down on (Vibepollo takes a release only from the client that pressed).
- `compositor.c` sends a remote screen's pointer events to its stream. A click on a remote screen takes the typing there (input relay keys and our VR keyboard), and leaving releases the keys it held (`blur`).
- `--beside` runs a second ft-screens next to the desktop for tests. It shows remote screens only and sends nothing to the input relay, ft-floatd or ft-layout.
- ft-pointer counts `frametop.remote.` panels as screens, and ft-gaze hit-tests them too (`remotes`, then `get N`). Without that, ft-gazed dropped looks below the keyboard pitch (-20°) on a low remote panel, as if you were looking at the keyboard.

**Tested through frame-testbench, with a `--beside` instance:**
- the OLED and a Remote Monitor as panels;
- Win+R, `notepad` and Enter typed through ft-screens' key path started Notepad on the test PC;
- Notepad dragged from the OLED panel to the Remote Monitor's stayed where it was dropped;
- the grab bar moves a remote panel;
- stopping the instance released the Remote Monitor.

**SteamVR's overlay limit.** SteamVR allows 128 overlays in the whole system (`k_unMaxOverlayCount`), its own included. Each Frametop panel takes six or seven with its controls. The desktop's floating-window slots made all of theirs at start: eight empty slots held 56. With three screens, a third remote screen got `VROverlayError_OverlayLimitExceeded`. A floating window's panel and controls are now made when a window floats on it, and destroyed when it docks (`EnsureFloatPanel`, `DropFloatPanel`). This needs a test on the live desktop. The controls of the other panels stay up, invisible until a laser comes near, because SteamVR's laser hover is what brings them in.

**Layouts, profiles and settings (2026-10-07, same branch):**
- `frametop-layout.json` has a `hosts` list. Each host has its displays: id, paired client, what it streams, label, stream size, fps, bitrate, and a screen's place, width, curve, pin and hidden flag. Each display keeps its screen number (101 and up), so the numbers don't shift when one is removed.
- ft-layout starts the remote displays' streams on every apply and at desktop start, even without a head pose, and places them when there is one. Displays without a place go in a row above the screens.
  - `capture` and `save` record their places.
  - A profile keeps their places and hidden state by id (`profiles[NAME]["remote"]`), and `use` puts them back.
  - `hide`/`show N` take their numbers.
  - `ft-layout remote list|monitors|add|set|connect|disconnect|remove`: `add` pairs the display's client with the host's token, and `remove` unpairs it (the host stays). `disconnect` sets the display's `off` flag and stops its stream; apply, profiles and desktop start skip it until `connect`.
- ft-screens starts a host's streams one after another. Three started at once made Vibepollo refuse some ("Another stream operation is still running"), and the captures that started while the Remote Monitor appeared got no picture.
- Frametop Remote Displays (`remote-displays/`) is the app for them. It was a Display Settings tab at first; the user wanted an app of its own for the connections. Display Settings' Screens page has a button that opens it.
  - Add a host with its token (written to `~/.local/share/frametop-stream/hosts/ADDRESS.token`, mode 0600). Whether it answers on its Web UI port (47990) is checked every 15 s.
  - Add a display: one of the host's monitors from its API, or a virtual one.
  - Per display: Connected, Shown, stream size, fps and bitrate (the stream starts over), width in VR, remove. Each host has a Connected switch for all of its displays. A lost stream shows why (`remote N info`).
- A disconnected display comes back where it was. ft-screens keeps a stopped remote panel's place, width, curve, pin and hidden state for the rest of its run (`Parked` in `vr.cpp`), and the next start of that screen restores them and replies `ok restored`. Otherwise ft-layout places it from the head, or with the headset off, from where the last arrangement was made, worked out from screen 1's place (`remote_anchor`).
- A vrcompositor crash: ft-screens freed a remote panel's texture imports while the panel still showed one. That happened at quit, at `remote stop`, and when a stream started over. vrcompositor crashed drawing it as the headset left standby. `ft_vr_forget` now clears a panel's texture before its import goes, and remote panels are destroyed before their buffers.

**vrcompositor crashed again at ft-screens quit (15:42, SIGBUS), and at 14:34.** Both times remote screens were up and the headset was in standby. SteamVR logs "leaving standby" within 2 ms of ft-screens disconnecting, so the compositor wakes in the middle of our cleanup, and it drew a texture that was already freed. The crash took the gamescope session and Steam down with it. The restart at 15:39 under the same conditions didn't crash, so it's a race. Hardened, not yet verified: ft-screens now tears SteamVR down first, while every buffer the panels show still exists (before KWin and the streams go). It clears the panels' textures, destroys the overlays, waits 200 ms, and only then releases the imports; nothing calls SteamVR after `VR_Shutdown`. Until that's checked, restart the desktop only with the compositor awake.

**Frozen panels, out-of-sight rate, sound (2026-10-07, after the user's test):**
- The OLED froze while a video played on it: frames came in at 59 fps and none were shown. ft-stream's message loop asked ft-screens' socket once more after the queue ran dry, to see whether it had closed, and lost whatever came in between. A lost `release` kept one of the three ring buffers from ft-stream for good, and after three nothing could be shown. Every pointer move is a message, so drags made it likely. (A lost button or key release would have stuck on the host the same way.) Fixed; ft-stream also takes the ring back if none of it comes back for 2 s.
- Out of sight, a stream used to decode only keyframes, one asked for each second. The user found that too aggressive. Every frame is now decoded, and 10 a second shown (`kHiddenFps`); coming back into view is immediate, with no keyframe requests (those are big frames, and the link already loses some).
- ft-stream ignored SIGTERM, ft-screens' death signal included: posix_spawn passed on ft-screens' blocked signals (its event loop takes SIGTERM, SIGINT and SIGCHLD through a signalfd). ft-screens now spawns it with none blocked, and ft-stream clears its mask too.
- Sound: ft-stream plays the host's sound (Opus, then PipeWire's PulseAudio server through libpulse-simple, about 40 ms queued). One stream per host plays it, the one holding `$XDG_RUNTIME_DIR/frametop-audio-HOST.lock`; the others try every 3 s, so another takes over when it ends. The host keeps playing its sound too. The test PC sent none: `remote_monitor_mute_audio = enabled` (our setup script) mutes the monitor and display roles. Turning it off needs an admin change on the test PC and a Vibepollo restart.

**Where the lost frames went, and the dongle (2026-10-07, ~16:40):**
- With the streams on the home network, about one frame every 2 s (all three together) was unrecoverable: bursts of 5 to 14 packets of one frame lost, beyond what FEC (Vibepollo's default 20%) repairs, and each loss then waited for a keyframe.
- Not on the Frame: UDP `RcvbufErrors` and `InErrors` stayed 0, wlan0 rx drops 0, the driver's misc drops +6 in 90 s. Not on the test PC: I225-V outbound discards and errors 0. The Frame's link was fine (-28 dBm, 2.1 Gbit/s, power save off). So the router loses them, as `~/.cache/wifitest` found for Steam Link VR (2026-10-01).
- Same burst test (vrsim, 50 Mbit/s at 60 fps, 20 s) from the test PC: through the router 0.33% of packets and 21 of 1200 frames lost (worst frame 21 ms); over the test PC's Steam Link dongle on the Frame's hotspot (the PC on the Frame's hotspot, the Frame at 10.35.78.1), none (worst 5.6 ms).
- So a host can go over its dongle. Its entry in `frametop-layout.json` has `direct` (the dongle's address) and `route`: `auto` (the default: the dongle when its port 47989 answers within 300 ms, else the network), `network` or `dongle` (only: while it's down, the stream is "lost the dongle link is down" and ft-screens tries again). ft-stream reads them itself, so ft-screens passes the host's own address as before. Its state says which way it went (`live via dongle|network`, `remote N info`). Remote Displays has a Connection setting per host, the dongle's address, and Find, which looks for the host among the hotspot's clients (`/proc/net/arp`, device `wlanap`) with the same `<uniqueid>` in its serverinfo. `ft-layout remote host NAME route=... direct=...` saves it and starts the host's running streams over in place.
- On the dongle, none of the three streams lost a frame in 2 to 3 minutes each, all at 60 fps.
- Sound: `remote_monitor_mute_audio = disabled` on the test PC (backup `D:\remote-displays\vibepollo-setup\sunshine.conf.before-audio-20261007-163556`, ApolloService restarted). One stream plays it (`Remote display: ADDRESS` in PipeWire, through SteamOS's spatial filter chain to the headset's speakers). That stream uses about 5% of a core more than the others (240-sample `pa_simple_write`s; batching them would help).
- Seen on the way: a stream that started while Vibepollo restarted chose the network (the dongle didn't answer yet) and then got no video, only sound; starting it over fixed it. Starting one display's stream once ended another's ("lost", back 5 s later). Vibepollo takes about 5 s to answer serverinfo, so every stream start waits that long.

**Adding a computer: sign in instead of carrying a token (2026-10-07, ~17:00):**
- Before, a host's API token came from `make-frametop-token` run on the host (its Web UI login typed there), and the file had to reach the Frame by hand. Now Remote Displays does it. Add computer lists the Vibepollo and Sunshine computers that announce `_nvstream._tcp` over mDNS (`avahi-browse -rpt` on the host; a computer also seen on the hotspot, `wlanap`, has a dongle, and that address is kept as its `direct`), or takes an address. You sign in once with the host's Web UI user name and password. Frametop posts them (HTTP Basic) to `/api/token` for a token with only what it uses: `/api/pin` POST, `/api/clients/list` GET, `/api/clients/update` POST and `/api/display-devices` GET (not `/api/clients/display-layout`, which the old script allowed). It keeps the token, mode 0600, and forgets the password. The sign-in goes over the dongle when there is one. Then Add displays lists the host's monitors, all ticked but those already added, and a virtual display.
- Hosts without a dongle use the network, and their card says so, with Find a dongle; the Connection choices only show once a dongle is known. A laptop without one was the test: it's on the network and not on the hotspot.
- The Web UI's certificate is self-signed. Its public key is pinned at the first sign-in (`hosts/ADDRESS.pin`, `sha256//...` as curl takes it): ft-stream's API calls set `CURLOPT_PINNEDPUBLICKEY`, and a later sign-in refuses another key ("If Vibepollo was installed again, remove the host and add it again"). The test PC's key is the same on both paths. Checked: the right pin works, a wrong one fails. The test PC's existing token got its pin too.
- Checked without the password: discovery found the test PC (its LAN address and its dongle on the hotspot, marked added) and a laptop; a wrong password says "Wrong user name or password". A real sign-in is the user's to try (the password never goes through chat).
- Sign in again (on a host card) makes a new token; the old one stays in the host's Web UI under API Tokens until revoked there (Frametop's token can't revoke tokens).

**The PC side: Frametop host setup (2026-10-07, ~17:40):**
- `host/windows/Setup Frametop host.cmd` (it runs `frametop-host-setup.ps1` and asks for admin) does what the test PC got by hand, nothing else of its settings:
  1. Vibepollo 2.0.0 with its own installer when it isn't there (downloaded from Nonary/Vibepollo's release, SHA-256 checked first; you click through it).
  2. Frametop's build of `sunshine.exe` over the original (kept as `sunshine.exe.2.0.0-original`), SHA-256 checked: from `-FrametopBuild PATH|URL`, the script's `$BuildUrl`, or `sunshine-frametop.exe` next to it. Only over Vibepollo 2.0.0's own exe; another version stops it with a message. Without the build, virtual displays still work, but not the PC's own monitors.
  3. `remote_monitor_mute_audio = disabled`, `remote_monitor_disconnect_on_client_disconnect = enabled`, `remote_monitor_disconnect_on_stream_end = disabled`. The rest of `sunshine.conf` stays as it is.
  4. The Web UI login: keep the one there is, or set one (`sunshine.exe --creds`, the password typed into a hidden prompt and passed to it quoted).
  5. Checks: an inbound firewall rule for `sunshine.exe` on every network type (added if missing; Windows puts a Steam Link dongle's network in Public), whether a Steam Link dongle (an adapter "For Valve") is connected, and that the Web UI answers. It ends with what to pick and sign in as on the Frame.
  `-Check` only says what it would change, `-SkipLogin` leaves the login, `-Undo` puts back the original exe and the oldest backup of the settings. Backups and a log go to `%ProgramData%\Frametop`.
- Tested on the test PC (admin over SSH, `FRAMETOP_NO_PAUSE=1`): `-Check`; a run with nothing to change (no restart); `-Undo` (the original exe back) and a run with `-FrametopBuild D:\vp-build\src\build\sunshine.exe` (backed up, swapped, Vibepollo restarted, the streams came back on their own). Not tested: a PC without Vibepollo (the download and its installer), setting the login (needs the user at the PC), adding the firewall rule.
- Still to decide: where other PCs get Frametop's build. `$BuildUrl` is empty; publishing it needs the fork's source published with it (GPL-3.0).
- The Frame side: `install.sh` now builds ft-stream (`stream/build.sh`, step 7) with Remote Displays' menu entry, and `uninstall.sh` offers to delete `~/.local/share/frametop-stream` (the clients' keys, the hosts' tokens and pins) with the other settings.
- Linux hosts (Vibepollo's Arch package): a host setup like this one, later.
- After Vibepollo restarted, a stream checked the dongle in 300 ms, too soon, and went over the network; ft-stream now gives it a second.

**Tested in the live desktop (frame-testbench, the 3D mouse through `@ft_pointer_helper`, 2026-10-07):**
- An Explorer window carried by its title bar from the Remote Monitor to the OLED stayed there, and carried back, stayed there too. The log showed one change of screen each way.
- Disconnect, connect: the panel came back where it was, at its width. Connect in a new run with the headset off: placed from screen 1's anchor.
- Windows rescales a window that moves between displays with different scaling, so the point you held moves on the window. A second drag from the same spot can land on the address bar instead of the title bar. That's Windows, not the routing.

**Still to do:**
- A headset test of all of it in the live desktop:
  - the row above the screens;
  - moving a remote display and saving a profile, then `use` putting it back;
  - adding a display from Remote Displays;
  - drags across with a controller, and gaze on the remote panels.
- The shutdown hardening above, verified: restart the desktop with remote screens up and the headset in standby (a crash takes the gamescope session down, so only with the user's OK).
- The floating windows' panels made on demand, tested live.
- Sound in the headset, by ear: one copy, in time with the picture.
- Over the network, a lost frame still waits for a keyframe. Reference frame invalidation (moonlight-common-c's `CAPABILITY_REFERENCE_FRAME_INVALIDATION_HEVC`) would recover without one, if the decoder copes.
- Find the dongle again if its address changes (the hotspot's DHCP), and a host whose own address changes (the token and pin files are named by it).
- "Approve on the PC" instead of the password (our Vibepollo fork), later if wanted.
- A per-display mouse mode (relative, for games).
- A picture for a stream that's connecting or lost.
- The installer doesn't build ft-stream yet (`stream/build.sh`), and `uninstall.sh` leaves `~/.local/share/frametop-stream` (the client keys and host tokens).
- For the test, the pointer and gaze services run this branch's builds through drop-ins (`~/.config/systemd/user/frametop-{pointer,gaze}.service.d/remote-displays-worktree.conf`; `gaze/tracker/build` here links to the installed tracker's). Remove them when this is merged and installed.

## Open questions

- Which of the test PC's two desk monitors is the real one (the main session)?
- The Mac's chip (Max chips have two video encode engines) and BetterDisplay Pro matter only once the Mac goes past one display.
- Audio: none, the focused display's host, or a fixed one?
- Does a remote display belong to a profile, so switching profiles connects and disconnects streams?
- Remote displays outside the desktop: should they also show over games, where the decoder is shared with Steam Link VR?
