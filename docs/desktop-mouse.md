# Independent desktop mouse

`input/ft-mousectl desktop --save` selects normal desktop input: the physical
mouse moves across the nested KDE monitors, clicks and scrolls through KDE's
Wayland seat, and displays KDE's own cursor. No mouse event becomes a virtual
controller event. Physical controllers remain available to SteamVR and games.
While a routed mouse is connected, controller events on desktop app panels are ignored;
floating app panels, the separate grab bars and placement controls still work under the existing
controller policy. SteamVR dashboard controls are unaffected.

Native CLI commands (no additional project tooling required):

```sh
input/ft-mousectl status
input/ft-mousectl desktop --save
input/ft-mousectl center 2
input/ft-mousectl position 2 960 540
input/ft-mousectl layout
input/ft-mousectl spatial --save
```

Numbers follow KDE's WL-0, WL-1 output order, starting at 1. Positions are local
logical pixels; `layout` reads actual output geometry and scale from the running
private KDE session. Geometry follows FrameTop layout changes too. A drag keeps
the Wayland implicit grab on its initial output until all buttons are released,
while the cursor can cross onto another monitor.

The relay gathers motion and wheels until the mouse's SYN_REPORT. High resolution
wheel reports use 1/120-notch units and replace accompanying ordinary reports;
they are delivered immediately without the spatial pointer's pulse timer. The
main and thumb wheels are independent. Standard mouse button codes are passed
through, including back/forward; spatial mouse button mappings do not apply.
`DESKTOP_MOUSE_SPEED` in `~/.config/frametop.conf` sets logical pixels per motion
count, default 1, range .05–10. This initial implementation uses constant speed,
not KDE's libinput acceleration settings, because FrameTop's relay owns the
physical device outside the nested session.

The cursor overlay only displays KDE's cursor; it has no input method, laser
claim, or interactive overlay flag. Cursor shape, hotspot and buffer scale come
from KDE. GPU cursor buffers use the same zero-copy DMA-BUF import as the monitor
textures. Common 32-bit shared-memory cursor formats are converted to RGBA.

Controller fallback: `DESKTOP_CONTROLLER_FALLBACK=1` (default) allows controller
laser input into desktop apps when no grabbed physical mouse is routed by the
relay. Idle mice do not trigger fallback. The relay reports presence every
second; absence must persist for one second, so disconnect detection normally
takes about one to two seconds. Keyboard routing still follows desktop clicks.
The virtual spatial mouse/gaze pointer stays off; controller policy stays
`dashboard`, preserving SteamVR's existing pointer-mode behavior.

Reconnect or native mouse motion returns ownership to the native seat and
releases controller-held buttons before clearing its focus. A held native mouse
button prevents fallback until released. An unknown presence or a heartbeat
older than three seconds blocks automatic controller desktop input. Set
`DESKTOP_CONTROLLER_FALLBACK=0` to retain strict separation even without a mouse;
reload the relay at an appropriate pause after changing this preference.
`input/ft-mousectl status` includes `controllerFallback` with presence, freshness,
configured enablement and actual desktop controller ownership. Older running
compositors report this field unavailable until explicitly reloaded.
Both paths support images up to 512×512 and buffer scale 8. FrameTop retains the
image from the first surface commit, because KDE can commit it before selecting
the active cursor and wlroots releases current.buffer after commit callbacks.
An unsupported buffer is reported by cursorReady=false with cursorError
and hides the old cursor image. Rendering in VR still needs a wearer check for
visibility, stereo depth, monitor edges and cursor shape changes.

The private desktop and apps launched from it use KDE's `kcminputrc` cursor
preferences (24px Breeze by default). Steam's VR environment exports
`XCURSOR_SIZE=256` and `XCURSOR_THEME=steam`; inheriting these makes app cursors
huge and can make their appearance change across monitors. A live KDE
CursorChanged notification updates KDE, but already running apps may retain
the old environment until reopened. Do not resize the VR overlay to mask an
app's incorrectly configured cursor size.

The updated compositor, pointer helper and relay must all be loaded before
activation. `status` is read-only and shows readiness, actual/desired mode,
held buttons and delivery counters. Saving happens only after an acknowledged
mode change. Switching refuses while buttons/keys on a pointer device are held.
An absent compositor does not prevent explicit spatial recovery. Restarting the
desktop requires the user's approval; use `desktops.sh restart` so background
work is preserved, and restore the current app windows afterwards.

If the compositor is lost, native mouse messages are not redirected into VR.
The input helpers need reloading when upgrading this feature. A standalone
helper/compositor restart may require `input/ft-mousectl desktop` again; inspect
all three components in `status` rather than inferring success from motion alone.

Offline verification (or run `scripts/test-desktop-input.sh`):

```sh
python3 -m unittest discover -s input -p 'test_*.py'
scripts/frame.sh -C screens 'gcc -std=c11 -Wall -Wextra -Werror tests/desktop-mouse-test.c -lm -o build/desktop-mouse-test && build/desktop-mouse-test'
scripts/frame.sh -C screens 'gcc -std=c11 -Wall -Wextra -Werror $(pkg-config --cflags libdrm) tests/desktop-cursor-test.c -o build/desktop-cursor-test && build/desktop-cursor-test'
scripts/frame.sh -C screens 'gcc -std=c11 -Wall -Wextra -Werror tests/cursor-cache-test.c $(pkg-config --cflags --libs wlroots-0.20 wayland-server wayland-client libdrm pixman-1) -o build/cursor-cache-test && build/cursor-cache-test'
```

The direct seat and cursor path follow the [Wayland pointer protocol](https://wayland.freedesktop.org/docs/html/apa.html#protocol-spec-wl_pointer).


The default remains `MOUSE_MODE=spatial`. Native mode is explicitly selected
with `input/ft-mousectl desktop --save`; no driver bindings or SteamVR defaults
are changed by installation. The new native mode suppresses virtual gaze, hand
gesture and gaze-click input while selected. Switch back to spatial mode to
use those features. Ordinary VR keyboard events remain available.

## Experimental desktop handoff

`input/ft-mousectl policy last-active` enables shared desktop input. Controller
clicks and nonzero scrolls inside desktop monitors take ownership; aiming alone
does not. Native mouse motion, clicks and scrolling reclaim ownership. Same
millisecond claims favor the mouse. Held buttons keep ownership until released;
other-device events are discarded rather than replayed after the drag. SteamVR
controls and separate monitor grab/placement bars do not participate.

`input/ft-mousectl policy mouse` restores the original mouse-first behavior
(including configured mouse-absent fallback). `policy pointer` reserves desktop
input for the controller. Changes refuse while either device holds a desktop
button. `status` reports policy, owner, held buttons and ignored mouse events.
These choices are runtime-only and reset to `mouse` when the desktop restarts.
The original PR checkpoint is retained on `checkpoint/desktop-input-before-handoff`;
this experiment is developed separately on `feat/desktop-input-handoff`.
