// ft-pointer: the universal 3D mouse's brain (OpenVR overlay client, runs in the dev container).
//
// input-relay.py (pointer mode) sends mouse commands here; this program keeps the
// cursor, does collision against SteamVR's overlays, draws the free-space dot, and
// sends the ft_pointer driver the exact pose of its virtual controller.
//
//   relay -> @ft_pointer_helper -> ft-pointer -> @ft_pointer -> ft_pointer driver (inside vrserver)
//
// Cursor model:
//   - anchor: head position at the last recenter; yaw/pitch: direction from it (mouse-driven).
//   - Every frame a ray from the anchor is tested against every visible overlay
//     (ComputeOverlayIntersection). On a hit the cursor sits on that surface; otherwise
//     it floats `distance` metres out and a small dot overlay is shown there, which the
//     laser can hit, so SteamVR never draws a free-flying laser.
//   - Then the line of sight from the eye (not the anchor) to that point is tested too:
//     after the head moves, something nearer can cover the point, and the cursor goes on
//     whatever you see under it (panels close together in view, at different depths).
//   - Looks: the compositor ignores live changes to dashboard.laserRayWidthScale (only
//     the dashboard's own Settings screen reloads it), so the beam can't be switched
//     off per device. Instead the laser starts POINTER_ORIGIN_FRACTION (0.95) of the way
//     from the eye to the cursor, along the line of sight: what's left of the beam is a
//     few centimetres long and effectively invisible, and SteamVR's hit dot (sized by
//     distance from the origin) becomes tiny. Our own white dot is the visible cursor
//     everywhere: a non-interactive dot on panels (the laser passes through it), and an
//     interactive one in free space (the laser lands on it instead of flying off).
//   - The controller ray starts at the eye and aims at the cursor point. Everything here
//     is computed in the standing universe; the pose sent to the driver is converted to
//     SteamVR's raw tracking space (drivers report raw poses; on the Frame the standing
//     origin is ~1.6 m above the raw one, so sending standing coordinates put the laser
//     origin 1.6 m above the head). While our device
//     owns the dashboard pointer, dashboard.laserRayWidthScale is 0 so only the dot shows.
//     It's restored when a controller takes the pointer back.
//
// Headset off: when SteamVR says nobody is wearing the headset, the pointer is released and
// stays off until it's worn again, so the displays can sleep (see the main loop). While the
// pointer is off the helper also stops listing overlays with vrcmd, whose connection every
// second kept SteamVR from going to standby.
//
// Last used wins: when a real controller moves (picked up), the pointer is released
// (driver "hide", which also drops its hand role hint), so the controller gets
// its role and laser back. The next mouse input reconnects and claims the laser again.
// Moving means faster than 0.35 m/s or 2 rad/s, both times POINTER_CONTROLLER_PICKUP,
// for 100 ms in a row with the controller tracked normally: a single sample over the
// limit was enough before, and controllers resting on a desk released the pointer on a
// knock or a tracking jump.
// SteamVR gives a contested hand role to the most recently used device, and a held
// Frame controller counts as used (touch sensors). If our device hasn't got the hand
// role within a second of waking, the pointer is released (no orphan white dot) and
// mouse input can't wake it again for 2 s.
//
// Laser mode: with the dashboard closed, SteamVR keeps its laser mouse off until a
// click (the first click on a panel only turned it on, the second one clicked), and a
// laser that leaves every panel turns it off again. While the pointer is awake, the
// helper shows frametop.pointer.lasermode: a transparent 1 mm overlay 50 m below the
// head with VROverlayFlags_MakeOverlaysInteractiveIfVisible, which keeps SteamVR's
// laser mouse mode on as long as it's visible. It's hidden whenever the pointer is
// released, so controllers and VR games get the normal behaviour back.
//
// Tilt: while the left button is held (dragging a panel by its grab bar, which SteamVR
// moves rigidly with the controller), pressing the right button enters tilt mode. The
// right press is not forwarded; mouse motion then rotates the virtual controller around
// the grab point (horizontal: about the vertical axis, vertical: about the view's
// horizontal axis), so the panel turns around that pivot. The tilt accumulates for the
// whole drag: after the right button is released, the rotation stays applied (about the
// moving cursor point) so the grabbed panel keeps its new orientation, and pressing right
// again continues from it. Releasing the left button drops the panel; the tilted pose (and
// the drag lock) are held 0.5 s longer, because SteamVR's dashboard finishes a floating
// move up to 150 ms after the release (UndockedOverlay.endFloatingWindowMove measures the
// push distance first) and reads the controller pose again then.
//
// Scene-graph overlays: the dashboard's dock (valve.steam.gamepadui.bar) and the controls
// under floating windows (valve.steam.gamepadui.floatingfooter, undock and friends) have
// no texture (0x0) and a placeholder width, so ComputeOverlayIntersection never hits
// them. For those the ray is tested against the overlay's plane, within
// POINTER_SCENE_RADIUS (0.5 m) of its origin; the laser-catching dot sits 5 cm behind the
// plane, so the laser reaches the buttons and still lands on the dot between them.
// Only absolutely placed 0x0 overlays count as scene-graph: gamescope's app panels (the
// desktops) also report 0x0, but they're placed as dashboard tabs, stay up when the
// dashboard closes, and ComputeOverlayIntersection hits them normally.
//
// SteamVR Settings (a workaround for that page only): Steam's pages (Library and the rest)
// are drawn in valve.steam.gamepadui.main, a dashboard overlay ComputeOverlayIntersection hits
// exactly. SteamVR's Settings page isn't: the main overlay is hidden, and the page is drawn by
// the scene-graph panel (valve.steam.gamepadui.frame.menu.N), whose shape OpenVR doesn't give
// out. Its transform's plane isn't the page's surface, which is nearer, so the laser, starting
// a few cm in front of where we thought the page was, started behind it: most of the page
// took no clicks, which went through to a desktop screen behind, and the page covered our
// dot. So while the cursor is on that page (OnSettingsPage), the laser starts near the eye
// (SETTINGS_ORIGIN) and SteamVR's own hit test finds the page; our dot is drawn close in front
// (SETTINGS_DOT), and the laser-catching dot sits far behind everything (SETTINGS_CATCHER),
// invisible and with SteamVR's hit dot hidden, so it can't cover the page. The beam and
// SteamVR's hit dot on the page then look like a controller's. Everywhere else nothing
// changes.
//
// Panel edges: off a panel, the cursor stays on that panel's plane while it's within
// POINTER_EDGE_REACH (0.3 m) of the last point it touched, instead of jumping to
// POINTER_DISTANCE. A floating panel's resize margins and the window controls under it
// sit just outside the panel, and the laser has to start in front of that plane to reach
// them (a controller's laser always does: it starts at the hand). Like on scene-graph
// planes, the laser-catching dot sits 5 cm behind the plane.
//
// Drag lock: while the left button is held, the cursor keeps the distance it had at the
// press and collision is frozen, so dragging past a panel's edge (resizing, moving)
// doesn't jump the cursor to free space or swap in the laser-catching dot, which made
// SteamVR's resize snap back.
// A left release also goes to ft-screens ("up"), which releases a button held on its
// screens in KWin if SteamVR gave the release to some other overlay.
// Across Frametop's panels (a drag and drop, or a window moved from one screen or floating
// window to another), the lock gives way: while the left button is held on one of ft-screens'
// panels, the ray is still tested against ft-screens' panels, and the cursor goes onto
// whichever it meets first. Not while that panel is being carried (its title bar or its bar):
// then the ray would find what's behind it.
//
// Head follow (experimental, off by default; POINTER_FOLLOW=1, or the relay's "follow toggle"): the cursor
// is carried by a reference direction, where the head faced when it last settled, and turns
// with it, keeping its offset (mouse movement changes the offset, up to POINTER_FOLLOW_REACH,
// 70 deg, so the cursor can sit in a corner of the view). While the head stays within
// POINTER_LEASH_DEG (10) of the reference, nothing moves on its own: the cursor stays put in
// the room. Once the head has been past the leash for POINTER_LEASH_DELAY (0.2 s; a glance
// out and back doesn't count), the reference follows: it eases toward the head's facing with a
// time constant of POINTER_LEASH_RETURN (0.2 s), never falling further behind than the leash
// (or than it already was), until it lands on the facing, and the cursor is back where it was
// in the view. Then it waits for the leash again. Earlier tries: dragging the reference only at
// the leash's end left it up to the leash off after turning back (getting it centred took an
// overshoot), and easing it all the time moved the cursor on every small head movement. At 0
// the reference is the head's facing, so the cursor is head-locked. Head roll is ignored (the
// frames have no roll), so tilting the head doesn't swing the cursor. The ray origin (the
// anchor) moves to the eye with the reference, so leaning inside the leash doesn't move the
// cursor either. While the left
// button is held (and the drop hold after it), the leash still moves the reference but the
// cursor stays put in the room, so a click or a drag can't be nudged by the head; the offset
// is taken up from where the cursor is when the hold ends, so it doesn't jump.
//
// Gaze mode (experimental, off by default; POINTER_GAZE=1, "gaze on|off|toggle", or the
// relay's gaze_toggle): the pointer goes where you look, and the mouse does the last bit
// (MAGIC pointing: Zhai, Morimoto and Ihde, CHI 1999). The gaze service (gaze/ft-gazed)
// sends the corrected gaze 90 times a second, "gz <yaw> <pitch> <raw yaw> <raw pitch>"
// (head-relative degrees), and while the gaze has the pointer, the cursor ray is simply
// that gaze from the eye: nothing is steered, so nothing can pile up. Moving the mouse takes
// the pointer from the gaze, and it moves from where the gaze left it, as usual. Looking
// well away from it (more than POINTER_GAZE_RETAKE, 5 deg, for 120 ms, with the mouse still
// for 300 ms) gives it back to the gaze; small eye movements around the pointer don't.
//   A left press while the gaze has the pointer isn't sent yet: the pointer stops where the
// gaze put it, and if the gaze is off, you drag it onto what you meant with the mouse
// (still holding the button; panels only see it hover). The release clicks there, a press
// and a release 40 ms apart. Held still for POINTER_GAZE_HOLD (0.5 s) instead, it becomes
// a real press where the pointer is, so drags work: hold, then move. After a click that
// didn't need correcting, and after a drag, the gaze has the pointer again.
//   Outside games (no scene application), gaze mode keeps the pointer: the relay doesn't
// release it when the mouse is idle ("gazeawake 1|0" tells it). A controller that moves
// still releases it, as without gaze (last used wins), and in games the mouse wakes it and
// idling releases it, as without gaze. Gaze mode is a mouse feature: the controllers aren't
// part of it. Steam reads the Frame controllers itself, outside SteamVR's bindings, so
// controller clicks at the gaze can't be done cleanly (docs/gaze-controllers.md).
//   The dot shows all the time in gaze mode (POINTER_GAZE_DOT=always, the default). With
// POINTER_GAZE_DOT=moving it only shows while the mouse moves it (within POINTER_GAZE_SHOW,
// 1 s), while a press is held, and briefly for each click (a pulse); otherwise it's
// transparent (still there for the laser to land on), since you know where you're looking.
//   When the mouse took the pointer and you then click, the nudge was probably onto what
// you were looking at: from the raw gaze when the mouse took over to where you clicked is
// the tracker's error there. The helper sends it to ft-gazed as a lesson ("lesson <raw yaw>
// <raw pitch> <true yaw> <true pitch>", the true direction relative to the head as it was
// when the mouse took over) if the mouse moved at least 0.2 deg, the click came within 10 s,
// and the correction (raw gaze to click) is within POINTER_GAZE_NUDGE_MAX (55 deg, half what
// the headset shows across: the Frame's eyes see 109 deg each). Past that, the tracker is far
// off (or you went somewhere else with the mouse): it isn't learned, and ft-gazed is asked
// for its quick check instead ("recheck <deg>"; it waits out its cooldown). Our tracker's
// clicks since its calibration put a one-dot check within 15 deg for the next 2 minutes and
// 25 for the next 10, so the check gets back under it (2026-10-01). A held press dragged onto
// the target is the same: from the raw gaze at the press to the release. With no
// fresh gaze (a blink, the service stopped, the headset off), the pointer stays put.
//
// Gaze precision (the relay's gaze_precision and gaze_drag actions, bound to a mouse button or
// a key combination; "precision|gazedrag mouse|keyboard 1|0" here): gaze mode aims, the button
// makes it exact. Pressing gaze precision stops the pointer where you look, as gaze mode's
// mouse press does; while it's held, the mouse steers the pointer by its moves. Releasing
// clicks where the pointer is. A correction is a lesson for the gaze tracker, as with the
// mouse. Gaze drag is the same with a real press at once, dragging until the release, for
// title bars, grab bars, and selections. Without gaze mode both still work from wherever the
// pointer is.
//   In gaze mode the mouse's left button is a gaze precision button (POINTER_GAZE_MOUSE =
// precision, the default), or clicks at once where the pointer is (direct). With precision the
// mouse's buttons work like the keyboard clicks: the right button's press is held back the same
// way, and the right click comes on the release, where the pointer is by then (gaze_right).
// Pressing the right button while the left one's press is held back presses the left button
// where the pointer is now, and the mouse drags (gaze_left then gaze_right): the drag lasts
// while either button is held. So once you've moved the pointer, the left button alone only
// clicks; to drag from there, press the right one. Pressing the right one again (a double right
// click, with the left still held) tilts, as a right press does during any drag (see Tilt).
// Held still for POINTER_GAZE_HOLD, either press is a real one.
//   POINTER_GAZE_MOUSE_MOVE: held (the default) or free. Held: while the gaze has the pointer,
// moving the mouse does nothing; it moves the pointer only during a press (as a correction,
// like a keyboard click's head). So a bumped or drifting mouse can't pull the pointer off what
// you're looking at, and every mouse move is a correction worth learning. With the gaze stale
// for a second (the tracker stopped, eyes closed), in a game, or the headset off, the mouse
// moves the pointer as usual. Free: the mouse takes the pointer whenever it moves.
//   Keyboard clicks (the relay's gaze_left and gaze_right, Meta+J and Meta+K by default;
// "gazekey left|right 1|0" here) work like gaze mode's mouse press, steered by the head: the
// press stops the pointer where you look, and while the keys are held the pointer stays put in
// your view, so turning your head carries it onto what you meant (past
// POINTER_HEAD_DEADZONE, 0.5 deg, so a still head doesn't wobble it). The release clicks there
// (left, or right for gaze_right), and a correction is a lesson for the gaze tracker, as with
// the mouse. Held still for POINTER_GAZE_HOLD instead, it's a real press, and the head drags.
// A quick tap (let go within POINTER_KEY_TAP, 0.25 s) clicks where the dot was at the press,
// whatever the head did meanwhile, and tells the gaze tracker it was right there (a lesson
// with no correction). Pressing gaze_right while gaze_left aims (Meta+K with Meta+J held)
// presses the left button where the dot is now, so you can correct first and then drag; the
// drag lasts while either key is held. gaze_right pressed during a gaze_left drag (held still
// into one, or again after starting one with it: a double Meta+K) tilts while it's held (see
// Tilt): turning the head turns the panel, and the mouse can too; let go of it and the head
// drags again from there. Without gaze mode they work from wherever the pointer is.
//   The gaze calibration panel (gaze/panel/ft-gazepanel, run by the gaze service): while
// ft-gazed says it's up ("calpanel 1", renewed every second; it lapses 3 s after the last),
// the dot hides and a press answers the panel instead of clicking: a left click or gaze_left
// sends "calaccept" to @ft_gazed (take this dot now), a right click or gaze_right "calquit".
//   POINTER_ROLE (right, left, or stylus): the hand role our device takes while connected. A
// Frame controller in your hand counts as used through its touch sensors and takes its hand's
// role back, and then no click lands (see "no hand role" in the main loop): with a controller
// held in the right hand, the pointer needs the left hand, or the stylus role.
//
// Hands (POINTER_HANDS, off by default; needs hand tracking, hands/): ft-hands publishes
// pinches and grips (hands/include/fh_gestures.h), read here every frame.
//   A pinch works like gaze mode's mouse press: the pointer stops (where the gaze put it), and
// the click comes when the pinch opens, where the pointer is then. A quick tap clicks where
// you looked. Held, the pinch's hand moves the pointer, for correcting the gaze: past
// POINTER_PINCH_DEADZONE (1.5 deg of hand movement, seen from the eye; a tap's jitter and the
// pinch point shifting as the fingers close stay inside it), at POINTER_PINCH_GAIN (0.5: half
// the hand's angle, for precision). A correction is a lesson for the gaze tracker, as with the
// mouse. A pinch ended by losing the hand (or by a grip taking over) doesn't click.
//   Without gaze mode, a pinch is a real press instead, like the mouse's button: pressed when
// it closes and released when it opens, and held, its hand drags the pointer (at the same gain
// and past the same dead zone). A tap is still a click where the pointer is. That's what the
// gaze probe's Click practice wants: it has its own gaze dot, frozen by the press and dragged
// by the pointer's movement until the release.
//   A grip (closing the hand) is a press and drag: the press where the pointer is, then the
// hand moves the pointer at POINTER_GRIP_GAIN (1: as far as it moves, seen from the eye), and
// opening the hand releases. So it drags whatever the pointer is on: a title bar moves the
// window, a panel's grab bar carries the panel, text is selected.
//   Typing touches thumb to index like a pinch: no pinch begins within POINTER_PINCH_TYPING
// (1 s) of a key (the relay says "typing"). A grip begins only with the hand held up, at most
// POINTER_GRIP_BELOW (0.35 m) below the eyes: hands on a desk curl like a loose fist. (The
// user's own pinches sat 0.35-0.45 m below the eyes, elbow resting, so pinches have no such limit.)
//   The hand's movement is taken in the room, from where the eye was when the gesture began,
// with the head pose at each frame's capture time, so turning your head doesn't move it.
// The first gesture while the pointer is off only wakes it. Gestures are ignored in a VR game
// (unless the dashboard is up) and with the headset off, and while the mouse's button is held.
// Hand use keeps the pointer from the relay's idle release for two minutes, as gaze mode does.
//
// Placement (for layout): SteamVR keeps a floating panel's position inside the
// dashboard, where nothing outside can set it, so the helper carries panels like a user
// would. It measures the panel (md::ScanPanel), aims the device at its grab bar
// (LAYOUT_GRAB_OFFSET, 7.5 cm below the bottom edge; the bands at 2-4 and 14-26 cm are
// other controls), presses, moves, and releases. While grabbed, the panel follows the
// device rigidly, except that the dashboard accelerates fast translations (0.1 m in 0.3 s
// moved it 0.19 m and turned it 8.5 deg, in jerky 25 ms steps right after the press). So
// the device hovers first, and the move is split into a rotation about the device origin
// (the eye) at 60 deg/s and a smooth 60 Hz slide at LAYOUT_SLIDE_SPEED (0.5 m/s; tested
// exact from 0.07 to 1 m/s). Scroll pushes along the panel normal, but only in whole notches of
// about 7 cm, so it isn't used. The result is measured again, and the move repeated up to
// twice while it's more than 1.5 cm or 1 deg off.
//
// Ignored panels: overlays matching POINTER_IGNORE are left out of the collision, so the cursor
// passes through them to what's behind. For display-only panels in the way, such as a
// head-locked performance overlay, which ComputeOverlayIntersection hits like any other. The
// laser starts just before the cursor point (see Looks), so a panel nearer to you doesn't
// catch it either.
//
// Commands (datagrams on @ft_pointer_helper): show, hide, recenter, move <dyaw> <dpitch>,
// follow on|off|toggle (head follow, until the next restart or a change to POINTER_FOLLOW),
// gaze on|off|toggle|? (gaze mode, likewise with POINTER_GAZE; ? only asks), gz ... (the gaze, from ft-gazed),
// reload (re-read the settings below), debug (toggle a twice-a-second state log),
// overlays (replies with the overlay list as JSON, see OverlayList),
// vrbind/vrglobal/vrstatus (Frame controller buttons, see vrbuttons.h),
// and btn/scroll lines, which are forwarded to the driver unchanged. For layouts, with a
// reply datagram to the sender's (abstract) address:
//   place <overlay> <x> <y> <z> <yaw> <pitch> [roll [grab]]: centre in the standing
//     universe; the front faces back along the direction (yaw, pitch), turned by roll
//     (counterclockwise as seen, degrees) -> "ok ..." | "error ..."
//   measure <overlay> -> "ok cx cy cz width height xx xy xz yx yy yz zx zy zz" (centre,
//     size, and the panel's right, up, and front vectors)
//   head -> "ok x y z yaw pitch"
//   grabprobe <overlay>: log where below the panel SteamVR's laser hits something (to
//     find the grab bar again if a SteamVR update moves it)
//
// Settings (~/.config/frametop.conf): POINTER_DISTANCE (m, 1.5), POINTER_CURSOR_DEG
// (angular size of the dot, 0.4), POINTER_LASER_WIDTH (controller beam width to restore, 0.8),
// POINTER_ORIGIN_FRACTION (0.95): the laser starts this far along the eye-to-cursor line,
// but never closer than POINTER_ORIGIN_MARGIN (0.15 m) to the cursor point: SteamVR's
// small controls (undock, frame buttons) float a few centimetres in front of their
// panel, and a laser that starts behind them can't hit them. POINTER_FOLLOW (0) and
// POINTER_LEASH_DEG (10), POINTER_LEASH_DELAY (0.2 s), POINTER_LEASH_RETURN (0.2 s),
// POINTER_FOLLOW_REACH (70 deg): head follow, above. POINTER_GAZE (0), POINTER_GAZE_RETAKE
// (5 deg), POINTER_GAZE_NUDGE_MAX (55 deg), POINTER_GAZE_HOLD (0.5 s), POINTER_GAZE_DOT
// (always), POINTER_GAZE_SHOW (1 s): gaze mode, above. POINTER_CONTROLLER_PICKUP (1, 0.5 to 5): how hard a controller must
// move to take the laser back, above. POINTER_IGNORE (empty): ignored panels, above.
// POINTER_HANDS (0), POINTER_PINCH_GAIN (0.5), POINTER_PINCH_DEADZONE (1.5 deg),
// POINTER_GRIP_GAIN (1), POINTER_GRIP_BELOW (0.35 m), POINTER_PINCH_TYPING (1 s): hands, above.
// POINTER_GAZE_MOUSE (precision), POINTER_HEAD_DEADZONE (0.5 deg), POINTER_KEY_TAP (0.25 s),
// POINTER_ROLE (right): gaze precision and keyboard clicks, above.
#include <openvr.h>

#include "pointer_config.h"
#include "vrbuttons.h"
#include "vrmath.h"

extern "C" {
#include "../../hands/include/fh_gestures.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <climits>

#include <fcntl.h>
#include <fnmatch.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using namespace md;

// Hand gestures from ft-hands (see "Hands" at the top): /run/user/UID/frametop-hands/gestures,
// mapped read-only. Version 1 files have pinches only; their grips read as zero.
class HandGestures {
public:
    ~HandGestures() { Close(); }
    // A consistent copy of the file (its sequence lock), if it's there. Opens it, and
    // checks it's still the same file, at most once a second.
    bool Read(fh_gestures_t &out) {
        const auto now = std::chrono::steady_clock::now();
        if (now - checked_ > std::chrono::seconds(1)) {
            checked_ = now;
            const std::string path = "/run/user/" + std::to_string(getuid()) + "/frametop-hands/gestures";
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || size_t(st.st_size) != len_ || st.st_ino != ino_) {
                Close();
                const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (fd >= 0 && fstat(fd, &st) == 0 && size_t(st.st_size) >= offsetof(fh_gestures_t, grip)) {
                    void *m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
                    if (m != MAP_FAILED) map_ = m, len_ = size_t(st.st_size), ino_ = st.st_ino, ++opens;
                }
                if (fd >= 0) close(fd);
            }
        }
        if (!map_) return false;
        const auto *g = static_cast<const fh_gestures_t *>(map_);
        if (std::memcmp(g->magic, FH_GESTURES_MAGIC, 8) != 0) return false;
        const size_t n = std::min(len_, sizeof out);
        for (int tries = 0; tries < 3; ++tries) {
            const uint64_t seq = __atomic_load_n(&g->seq, __ATOMIC_ACQUIRE);
            if (seq & 1) continue;
            std::memset(&out, 0, sizeof out);
            std::memcpy(&out, map_, n);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&g->seq, __ATOMIC_RELAXED) != seq) continue;
            if (out.version < 2) std::memset(out.grip, 0, sizeof out.grip);
            return true;
        }
        return false;
    }
    int opens = 0;   // a new file: the counters start over

private:
    void Close() {
        if (map_) munmap(map_, len_);
        map_ = nullptr, len_ = 0, ino_ = 0;
    }
    void *map_ = nullptr;
    size_t len_ = 0;
    ino_t ino_ = 0;
    std::chrono::steady_clock::time_point checked_{};
};

// The HMD's recent poses (standing universe), to turn the gestures' head-frame points into
// the room as the head was when the cameras took them.
class PoseHistory {
public:
    void Add(std::chrono::steady_clock::time_point t, const vr::HmdMatrix34_t &m) {
        poses_.push_back({t, m});
        while (poses_.size() > 128) poses_.pop_front();   // about a second
    }
    // The pose at CLOCK_MONOTONIC time t_ns (steady_clock's), or the nearest kept.
    bool At(uint64_t t_ns, vr::HmdMatrix34_t &out) const {
        if (poses_.empty()) return false;
        const std::chrono::steady_clock::time_point t{std::chrono::nanoseconds(t_ns)};
        const auto *best = &poses_.front();
        for (const auto &p : poses_)
            if (std::chrono::abs(p.first - t) < std::chrono::abs(best->first - t)) best = &p;
        out = best->second;
        return true;
    }

private:
    std::deque<std::pair<std::chrono::steady_clock::time_point, vr::HmdMatrix34_t>> poses_;
};

int AbstractSocket(const char *name, bool bindIt) {
    const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (bindIt) {
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::memcpy(addr.sun_path + 1, name, std::strlen(name));
        const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name);
        if (bind(fd, reinterpret_cast<sockaddr *>(&addr), len) != 0) {
            std::perror("bind @ft_pointer_helper (already running?)");
            std::exit(1);
        }
    }
    return fd;
}

std::string ExeDir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    return p.substr(0, p.rfind('/'));
}

// One of ft-screens' panels showing a desktop: a screen (frametop.screen.N), a floating
// window (frametop.float.N), or a floating window's popup (frametop.float.N.sub.K), not a
// control of theirs.
bool FramePanel(const std::string &key) {
    for (const char *prefix : {"frametop.screen.", "frametop.float."}) {
        if (key.rfind(prefix, 0) != 0) continue;
        const std::string rest = key.substr(std::strlen(prefix));
        const size_t dot = rest.find('.');
        return dot == std::string::npos || rest.compare(dot, 5, ".sub.") == 0;
    }
    return false;
}

void SendTo(int fd, const char *name, const std::string &msg) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, name, std::strlen(name));
    const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name);
    sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr *>(&addr), len);
}

// JSON string literal (names come from other apps).
std::string JsonQuote(const std::string &s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
        } else out += char(c);
    }
    return out + "\"";
}

// Overlay keys, refreshed in the background from `vrcmd --overlays` (OpenVR has no
// public call to enumerate other apps' overlays). Hidden ones are listed too: the
// window controls under a floating panel only appear while something hovers the
// panel, and the cursor has to find them the moment they do, not a second later.
// Paused while the pointer is off: each vrcmd run connects to SteamVR as a new app, and a new
// app every second kept SteamVR (and the headset's displays) from going to standby.
// "overlays" requests (Frametop Input Settings' Ignored panels page) refresh the list even
// while paused, and are answered from this thread once it's fresh.
class OverlayList {
public:
    void Start() {
        out_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        thread_ = std::thread([this] {
            while (running_) {
                if (!paused_ || requested_) Refresh();
                // Wait a second, or less when the pointer wakes (refresh right away then).
                for (int i = 0; i < 10 && running_; ++i) {
                    const bool wasPaused = paused_;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if ((wasPaused && !paused_) || requested_) break;
                }
            }
        });
    }
    void SetPaused(bool paused) { paused_ = paused; }
    void Stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    std::vector<std::string> Keys() {
        std::lock_guard<std::mutex> guard(lock_);
        std::vector<std::string> keys;
        for (const auto &e : entries_) keys.push_back(e.key);
        return keys;
    }
    // Answer `to` with {"t":"overlays","list":[{"key","name","visible"}...]} after the next refresh.
    void Request(const sockaddr_un &to, socklen_t len) {
        if (len <= offsetof(sockaddr_un, sun_path)) return;
        std::lock_guard<std::mutex> guard(lock_);
        if (waiting_.size() < 8) waiting_.push_back({to, len});
        requested_ = true;
    }

private:
    struct Entry {
        std::string key, name;
        bool visible;
    };
    void Refresh() {
        requested_ = false;
        FILE *p = popen("LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null", "r");
        if (!p) return;
        std::vector<Entry> entries;
        char line[1024];
        while (std::fgets(line, sizeof line, p)) {
            // 'key' -- 'name', WxH visible VROverlayType_...
            if (line[0] != '\'') continue;
            const char *end = std::strchr(line + 1, '\'');
            if (!end) continue;
            const std::string key(line + 1, size_t(end - (line + 1)));
            const std::string rest(end);
            if (rest.find("Thumbnail") != std::string::npos || rest.find("Subview") != std::string::npos) continue;
            if (key.rfind("system.pointer", 0) == 0 || key.rfind("system.cursor", 0) == 0 ||
                key.rfind("frametop.pointer", 0) == 0 || key.rfind("frametop.guide", 0) == 0 ||
                key == "frametop.gazepanel" ||  // the gaze calibration panel, fixed to the headset
                key == "frametop.catcher" ||  // ft-screens' release catcher: only on a laser mid-drag
                key == "system.HeadsetView" || key == "system.toast")
                continue;
            // The name can hold quotes; it ends at the last "', " (the size and state follow).
            const auto nameAt = rest.find("-- '"), nameEnd = rest.rfind("', ");
            const std::string name =
                nameAt != std::string::npos && nameEnd > nameAt + 3 ? rest.substr(nameAt + 4, nameEnd - nameAt - 4) : key;
            entries.push_back({key, name, rest.find(" not_visible ") == std::string::npos});
        }
        pclose(p);
        std::vector<std::pair<sockaddr_un, socklen_t>> waiting;
        {
            std::lock_guard<std::mutex> guard(lock_);
            entries_ = std::move(entries);
            waiting.swap(waiting_);
        }
        if (waiting.empty()) return;
        std::string msg = "{\"t\":\"overlays\",\"list\":[";
        for (size_t i = 0; i < entries_.size(); ++i)
            msg += std::string(i ? "," : "") + "{\"key\":" + JsonQuote(entries_[i].key) + ",\"name\":" +
                   JsonQuote(entries_[i].name) + ",\"visible\":" + (entries_[i].visible ? "true" : "false") + "}";
        msg += "]}";
        for (const auto &[to, len] : waiting)
            sendto(out_, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&to), len);
    }

    std::thread thread_;
    int out_ = -1;
    std::atomic<bool> paused_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> requested_{false};
    std::mutex lock_;
    std::vector<Entry> entries_;  // written only by the thread; the lock guards readers
    std::vector<std::pair<sockaddr_un, socklen_t>> waiting_;
};

bool Ignored(const std::vector<std::string> &patterns, const std::string &key) {
    for (const auto &p : patterns)
        if (fnmatch(p.c_str(), key.c_str(), FNM_NOESCAPE) == 0) return true;
    return false;
}

vr::HmdMatrix34_t Billboard(Vec3 at, Vec3 eye) {
    // Overlay faces +Z; point +Z at the eye, keep +Y roughly up.
    const Vec3 z = Normalize(eye - at);
    const Vec3 x = Normalize(Cross({0, 1, 0}, z));
    const Vec3 y = Cross(z, x);
    vr::HmdMatrix34_t m{};
    const Vec3 cols[3] = {x, y, z};
    for (int c = 0; c < 3; ++c) {
        m.m[0][c] = float(cols[c].x);
        m.m[1][c] = float(cols[c].y);
        m.m[2][c] = float(cols[c].z);
    }
    m.m[0][3] = float(at.x);
    m.m[1][3] = float(at.y);
    m.m[2][3] = float(at.z);
    return m;
}

std::vector<uint8_t> DotTexture(int size) {
    // White dot with a dark rim, soft edge, transparent outside.
    std::vector<uint8_t> px(size * size * 4, 0);
    const double c = (size - 1) / 2.0, r = size * 0.42, rim = size * 0.10;
    for (int y = 0; y < size; ++y)
        for (int x = 0; x < size; ++x) {
            const double d = std::hypot(x - c, y - c);
            const double a = std::clamp(r - d + 0.5, 0.0, 1.0);
            const bool inner = d < r - rim;
            uint8_t *p = &px[(y * size + x) * 4];
            const uint8_t v = inner ? 255 : 40;
            p[0] = p[1] = p[2] = v;
            p[3] = uint8_t(a * 235);
        }
    return px;
}

// Unit vector v, turned toward unit vector `center` until it's at most maxRad from it.
Vec3 PullWithin(Vec3 v, Vec3 center, double maxRad) {
    if (std::acos(std::clamp(Dot(v, center), -1.0, 1.0)) <= maxRad) return v;
    Vec3 axis = Cross(center, v);
    if (Length(axis) < 1e-9) axis = Cross(center, {0, 1, 0});  // opposite: any perpendicular
    return RotateAbout(center, Normalize(axis), maxRad);
}

// Unit direction with its pitch limited to +-maxDeg (AimBasis needs it off vertical).
Vec3 LimitPitch(Vec3 d, double maxDeg) {
    const double pitch = std::clamp(std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI, -maxDeg, maxDeg);
    return Direction(std::atan2(-d.x, -d.z) * 180 / M_PI, pitch);
}

// SteamVR Settings page (see the top): the laser starts this far from the eye, the dot is
// drawn this far out, and the laser-catching dot sits this far out (metres).
constexpr double SETTINGS_ORIGIN = 0.25, SETTINGS_DOT = 0.6, SETTINGS_CATCHER = 8.0;

// Whether the line of sight from `eye` along `d` crosses the SteamVR Settings page, drawn by
// the dashboard's scene-graph panel (valve.steam.gamepadui.frame.menu.N, transform t); see
// "SteamVR Settings" at the top. The page has no size in OpenVR, so this is its area as
// measured on the Frame, generously: in metres from the panel's origin, which is near the
// page's left edge, it ran from about -0.35 (the sidebar) to 1.15 across and +-0.4 up and
// down, with the transform scaled 0.369. Kept in the transform's units so it scales with it.
bool OnSettingsPage(const vr::HmdMatrix34_t &t, Vec3 eye, Vec3 d) {
    const Vec3 c = Position(t), x{t.m[0][0], t.m[1][0], t.m[2][0]}, y{t.m[0][1], t.m[1][1], t.m[2][1]},
               z{t.m[0][2], t.m[1][2], t.m[2][2]};
    const double sx = Dot(x, x), sy = Dot(y, y), denom = Dot(d, z);
    if (sx < 1e-9 || sy < 1e-9 || std::fabs(denom) < 1e-6) return false;
    const double along = Dot(c - eye, z) / denom;
    if (along <= 0) return false;
    const Vec3 off = eye + d * along - c;
    const double u = Dot(off, x) / sx, v = Dot(off, y) / sy;  // in the transform's units
    return u >= -1.35 && u <= 3.4 && std::fabs(v) <= 1.25;
}

}  // namespace

// Placement speeds (see "Placement" at the top).
constexpr double kPlaceDegPerSec = 60;      // tested: 40 deg/s is applied exactly

using Clock = std::chrono::steady_clock;

// Everything the pointer keeps: its settings, what Init sets up, and the state the main loop
// carries from one frame to the next.
struct Pointer {
    PointerConfig cfg;
    // Head follow (see the top). followConf is POINTER_FOLLOW as last read: a reload only
    // overrides a "follow" command when the setting itself changed.
    bool follow = false, followConf = false, followReset = true;
    // Gaze mode (see the top); gazeConf is POINTER_GAZE as last read, like followConf.
    bool gazeOn = false, gazeConf = false;
    // Set in Init, after VR_Init.
    vr::IVRSystem *sys = nullptr;
    vr::IVROverlay *overlay = nullptr;
    // The dot, the marker, and laser mode's overlay, created in Init.
    vr::VROverlayHandle_t cursor = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t marker = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t laserMode = vr::k_ulOverlayHandleInvalid;
    bool laserModeShown = false;
    int in = -1, out = -1;  // @ft_pointer_helper, and the socket we send from (opened in Init)
    ControllerButtons controllerButtons;  // Frame controller buttons (vrbuttons.h)
    OverlayList overlays;
    std::map<std::string, vr::VROverlayHandle_t> handles;
    std::map<std::string, bool> sceneGraph;  // no texture: plane test instead of ComputeOverlayIntersection
    std::map<std::string, bool> visible;     // refreshed every 50 ms
    Clock::time_point lastVisible{};  // set in Init
    // The plane of the last panel the cursor was on, and the last point on it (panel edges).
    Vec3 edgePoint, edgeNormal, edgeLast;
    std::string edgeKey;

    bool active = false, recenter = false, anchored = false;
    Clock::time_point lastMouse{}, claimAt{}, claimRelease{}, wokeAt{}, noWakeUntil{};
    bool claimPending = false, claimHeld = false;
    // Last used wins: since when each controller has been moving (zero: it isn't).
    Clock::time_point movingSince[vr::k_unMaxTrackedDeviceCount] = {};
    // Tilt mode (see top of file).
    bool leftHeld = false, tilting = false, tiltStart = false, swallowedRight = false;
    // The mouse's buttons in gaze mode (see the top): leftDown, rightDown as the relay last said;
    // aimRight, the held-back press is the right button's; chordDrag, a drag the right button
    // began during the left's held-back press (the first release drops it, the other's is nothing).
    bool leftDown = false, rightDown = false, aimRight = false, chordDrag = false;
    bool ignoreLeftUp = false;
    // The keyboard clicks' keys as the relay last said, and a tilt gaze_right holds during a
    // keyboard drag (see the top): the head turns the panel, from where it was (keyTiltYaw, keyTiltPitch).
    bool keyLeftDown = false, keyRightDown = false, keyTilting = false;
    double keyTiltYaw = 0, keyTiltPitch = 0;
    double tiltYaw = 0, tiltPitch = 0;
    double dragDistance = 0, lastDistance = 1.5;  // drag lock: distance from the anchor at the press
    bool onVrSettings = false;       // the cursor is on the SteamVR Settings page (kept while dragging)
    bool catcherHidesHit = false;    // the laser-catching dot hides SteamVR's hit dot (Settings page)
    Clock::time_point dropHoldUntil{};  // after a left release: keep the drag pose this long
    bool debug = false;
    std::string lastHit;
    // The ft-screens panel the left button was pressed on, and where it was then (see the top).
    std::string pressKey;
    vr::HmdMatrix34_t pressPose{};
    Clock::time_point lastDebug{};  // set in Init
    vr::VROverlayHandle_t systemPointer = vr::k_ulOverlayHandleInvalid;  // found in Init
    Vec3 pivot, tiltOrigin, lastPoint, lastOrigin, lastAim{0, 0, -1};
    Basis tiltBasis{};
    bool headsetOff = false;  // nobody is wearing the headset (see the main loop)
    Vec3 anchor;
    double yaw = 0, pitch = 0;
    Vec3 followRef{0, 0, -1};  // head follow's reference direction (see the top)
    Clock::time_point followAt{};  // its last update, for the easing (set in Init)
    bool following = false;                           // past the leash: easing toward the head
    double followLag = 0;                             // radians the reference trails the head
    std::chrono::steady_clock::time_point leashOutSince{};  // head past the leash since (delay)
    // Gaze mode (see the top).
    struct Gaze {
        double hy = 0, hp = 0, rhy = 0, rhp = 0;  // corrected, and raw
        Clock::time_point at{};
    } gz;
    bool gazeOwns = true;  // the pointer follows the gaze; false: the mouse has it
    bool nudging = false;  // the mouse took it from the gaze: the next click may be a lesson
    double nudgeRawHy = 0, nudgeRawHp = 0, nudgeMoved = 0;
    vr::HmdMatrix34_t nudgeHead{}, lastHead{};
    bool haveHead = false, havePoint = false;
    Clock::time_point nudgeAt{}, retakeSince{};
    // A held-back press (see the top): aimHeld while the button is down; then the click
    // (clickPress at the end of the next frame, clickRelease 40 ms later). gazeBack: give
    // the gaze the pointer again when the press or click is over.
    vr::TrackedDeviceIndex_t ours = vr::k_unTrackedDeviceIndexInvalid;
    bool aimHeld = false, clickPress = false, clickRelease = false, gazeBack = false;
    bool aimHand = false;  // the held-back press is a hold's (below): it never turns into a real press
    Clock::time_point aimSince{}, clickReleaseAt{};
    // Holds (see "Gaze precision" and "Hands" at the top): a pinch or grip, or a gaze
    // precision or gaze drag button, held now. What steers the pointer meanwhile (a hand, or
    // the mouse through its own moves), from where it pointed when the hold began (for a
    // hand: seen from the eye then, in the room), and the pointer then.
    HandGestures handFile;
    PoseHistory poses;
    enum class Src { None, Hand, Mouse, Head };
    struct Hold {
        Src src = Src::None;
        int side = -1;          // a hand's side (0 left, 1 right)
        bool grip = false;      // pressed at once and dragging (a grip, gaze drag), not a click on release
        bool engaged = false;   // past the dead zone
        bool pressed = false;   // a real press went out (a grip or gaze drag, or a pinch without gaze mode)
        bool promote = false;   // held still for POINTER_GAZE_HOLD, it becomes a real press (keyboard clicks)
        bool right = false;     // the right button (gaze_right)
        bool keyDrag = false;   // gaze_right pressed while gaze_left aimed: a left press, either key ends it
        double pressYaw = 0, pressPitch = 0;  // the pointer at the press (a quick tap clicks there)
        Vec3 origin;
        double refYaw = 0, refPitch = 0, startYaw = 0, startPitch = 0, lastYaw = 0, lastPitch = 0;
    } hold;
    // "precision|gazedrag <source> 1|0" from the relay, done in the frame (see Holds).
    struct DevicePress {
        std::string source;
        bool drag, down;
    };
    std::vector<DevicePress> devicePresses;
    // "gazekey left|right 1|0" from the relay (keyboard clicks), done in the frame.
    struct KeyPress {
        bool right, down;
    };
    std::vector<KeyPress> keyPresses;
    uint32_t seenBegins[2][2] = {}, seenEnds[2][2] = {};  // [pinch, grip][side], as last read
    bool handBaseline = false;
    int handOpens = 0;
    uint64_t handSeq = 0, handPublished = 0;
    Clock::time_point handUsed{};  // a gesture began then (keeps the pointer, like gaze mode)
    Clock::time_point lastTyping{};  // the relay's last "typing": a key on a keyboard
    // Gaze mode outside games, and its dot (see the top): lastMove/lastHeld/pulseAt.
    bool inGame = false, gazeAwake = false;
    Clock::time_point inGameAt{}, gazeAwakeAt{};
    Clock::time_point lastMove{}, lastHeld{}, pulseAt{};
    // The left button, as sent to the driver; pressRight: the next press is the right button
    // instead (gaze_right), heldButton: the one pressed.
    bool pressRight = false;
    std::string heldButton = "trigger";
    bool confirmLesson = false;  // a keyboard click's quick tap: a lesson with no correction (see the top)
    // The gaze calibration panel is up until then ("calpanel 1"; see the top); calOpened: it just
    // came up, so a press in progress ends without a click.
    Clock::time_point calPanelUntil{};
    bool calOpened = false;
    Clock::time_point lastSlow{};  // set in Init

    void ApplyConfig();
    void Init();
    int Run();
};

// After cfg.Load: a changed POINTER_FOLLOW or POINTER_GAZE turns head follow or gaze mode on or off.
void Pointer::ApplyConfig() {
    if (cfg.follow != followConf) follow = followConf = cfg.follow, followReset = true;
    if (cfg.gaze != gazeConf) gazeOn = gazeConf = cfg.gaze;
}

void Pointer::Init() {
    cfg.Load(ReadConfig());
    ApplyConfig();
    const float laserWidth = float(ConfDouble(ReadConfig(), "POINTER_LASER_WIDTH", 0.8));

    vr::EVRInitError err = vr::VRInitError_None;
    while (true) {
        vr::VR_Init(&err, vr::VRApplication_Background);
        if (err == vr::VRInitError_None) {
            vr::VR_Shutdown();
            vr::VR_Init(&err, vr::VRApplication_Overlay);
        }
        if (err == vr::VRInitError_None) break;
        std::fprintf(stderr, "waiting for SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    sys = vr::VRSystem();
    overlay = vr::VROverlay();

    overlay->CreateOverlay("frametop.pointer.cursor", "Frametop pointer", &cursor);
    const int texSize = 64;
    auto tex = DotTexture(texSize);
    overlay->SetOverlayRaw(cursor, tex.data(), texSize, texSize, 4);
    overlay->SetOverlayInputMethod(cursor, vr::VROverlayInputMethod_Mouse);  // the laser can land on it
    overlay->SetOverlaySortOrder(cursor, 200);
    // Same dot, not interactive, drawn on panels at the hit point; the laser passes through.
    overlay->CreateOverlay("frametop.pointer.marker", "Frametop pointer marker", &marker);
    overlay->SetOverlayRaw(marker, tex.data(), texSize, texSize, 4);
    overlay->SetOverlayInputMethod(marker, vr::VROverlayInputMethod_None);
    overlay->SetOverlaySortOrder(marker, 201);
    // Laser mode (see the top of the file).
    overlay->CreateOverlay("frametop.pointer.lasermode", "Frametop pointer laser mode", &laserMode);
    std::vector<uint8_t> clear(4 * 4 * 4, 0);
    overlay->SetOverlayRaw(laserMode, clear.data(), 4, 4, 4);
    overlay->SetOverlayWidthInMeters(laserMode, 0.001f);
    overlay->SetOverlayInputMethod(laserMode, vr::VROverlayInputMethod_Mouse);  // the flag needs an input method
    overlay->SetOverlayFlag(laserMode, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    vr::HmdMatrix34_t below{};
    below.m[0][0] = below.m[1][1] = below.m[2][2] = 1;
    below.m[1][3] = -50;
    overlay->SetOverlayTransformTrackedDeviceRelative(laserMode, vr::k_unTrackedDeviceIndex_Hmd, &below);
    // Controller beams keep the user's width; nothing here changes it any more.
    vr::VRSettings()->SetFloat("dashboard", "laserRayWidthScale", laserWidth);

    in = AbstractSocket("ft_pointer_helper", true);
    out = AbstractSocket(nullptr, false);
    // Frame controller buttons (vrbuttons.h). The build puts the binary in pointer/helper/build.
    {
        const std::string manifest = ExeDir() + "/../actions/ft_pointer_actions.json";
        char real[PATH_MAX];
        controllerButtons.Init(realpath(manifest.c_str(), real) ? real : manifest);
    }
    SendTo(out, "frametop_relay", "vrhello");  // the relay answers with the mapped buttons
    overlays.Start();
    lastVisible = std::chrono::steady_clock::now();
    lastDebug = Clock::now();
    overlay->FindOverlay("system.pointer", &systemPointer);
    followAt = std::chrono::steady_clock::now();
    lastSlow = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    std::printf("ft-pointer running: free distance %.2f m, dot %.2f deg\n", cfg.freeDistance, cfg.cursorDeg);
    std::fflush(stdout);
}

int Pointer::Run() {
    auto wake = [&](Clock::time_point t) {
        if (t < noWakeUntil || headsetOff) return;
        wokeAt = t;
        active = true;
        recenter = true;
        SendTo(out, "ft_pointer", "role " + cfg.role);  // POINTER_ROLE, before it takes it
        SendTo(out, "ft_pointer", "show");
        claimPending = true;  // take the laser without clicking, once SteamVR has bound the device
        claimAt = t + std::chrono::milliseconds(300);
    };
    auto pressLeft = [&] {
        // ft-screens sends the keyboard to the panel clicked last; it sees clicks on
        // its own screens, but only we know when one lands on another panel.
        SendTo(out, "ft_screens", "click " + (lastHit.empty() ? std::string("-") : lastHit));
        // A click after nudging the gaze-placed pointer: the nudge is a lesson, or past
        // POINTER_GAZE_NUDGE_MAX, a quick check (see the top).
        if (gazeOn && nudging && !gazeOwns && havePoint && Clock::now() - nudgeAt < std::chrono::seconds(10) &&
            (confirmLesson || nudgeMoved >= 0.2)) {
            const Vec3 d = RotateInverse(nudgeHead, Normalize(lastPoint - Position(nudgeHead)));
            const double ty = std::atan2(-d.x, -d.z) * 180 / M_PI, tp = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI;
            const double off = std::hypot(std::remainder(ty - nudgeRawHy, 360.0), tp - nudgeRawHp);
            char msg[160];
            if (off <= cfg.gazeNudgeMax)
                std::snprintf(msg, sizeof msg, "lesson %.3f %.3f %.3f %.3f", nudgeRawHy, nudgeRawHp, ty, tp);
            else
                std::snprintf(msg, sizeof msg, "recheck %.0f", off);
            SendTo(out, "ft_gazed", msg);
            if (debug) std::printf("gaze %s (nudged %.2f deg, off %.2f)\n", msg, nudgeMoved, off);
            if (debug) std::fflush(stdout);
        }
        nudging = confirmLesson = false;
        leftHeld = true;
        dragDistance = lastDistance;
        pressKey.clear();
        if (FramePanel(lastHit)) {
            vr::ETrackingUniverseOrigin uo;
            auto it = handles.find(lastHit);
            if (it != handles.end() &&
                overlay->GetOverlayTransformAbsolute(it->second, &uo, &pressPose) == vr::VROverlayError_None)
                pressKey = lastHit;
        }
        tiltYaw = tiltPitch = 0;  // a new drag starts untilted
        dropHoldUntil = {};
        pulseAt = Clock::now();
        heldButton = pressRight ? "b" : "trigger";
        pressRight = false;
        SendTo(out, "ft_pointer", "btn " + heldButton + " 1");
    };
    auto releaseLeft = [&] {
        leftHeld = false;
        tilting = false;
        // Hold the drag pose (tilt, frozen distance) while SteamVR finishes the drop.
        dropHoldUntil = Clock::now() + std::chrono::milliseconds(500);
        SendTo(out, "ft_pointer", "btn " + heldButton + " 0");
        // ft-screens releases a button held on its screens in KWin even when SteamVR hands
        // the release to some other overlay (its catcher usually gets it; this is the backstop).
        SendTo(out, "ft_screens", "up");
        if (gazeBack) gazeOwns = true, gazeBack = false;
    };
    // The left button, from the relay's "btn trigger", with gaze mode's held-back press (see
    // the top).
    auto canAim = [&] {
        return gazeOn && cfg.gazeMousePrecision && gazeOwns && !aimHeld && !clickPress && !clickRelease &&
               hold.src == Src::None;
    };
    // Hold the press back: the pointer stops where the gaze put it.
    auto aimStart = [&](bool right) {
        gazeOwns = false;
        nudging = haveHead && Clock::now() - gz.at < std::chrono::milliseconds(200);
        nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
        nudgeAt = aimSince = Clock::now(), nudgeMoved = 0;
        aimHeld = true, aimRight = right;
    };
    // A drag the right button began (chordDrag): it lasts while either button is held.
    auto chordDrop = [&] {
        chordDrag = false;
        if (leftHeld) releaseLeft();
        if (debug) std::printf("mouse drag dropped\n");
        if (debug) std::fflush(stdout);
    };
    auto leftButton = [&](bool down) {
        leftDown = down;
        if (down) {
            if (aimHeld && aimRight && !aimHand) {
                ignoreLeftUp = true;  // the right's press is held back: the left does nothing
                return;
            }
            if (canAim()) {
                aimStart(false);
                return;
            }
            pressLeft();
            return;
        }
        if (ignoreLeftUp) {
            ignoreLeftUp = false;
            return;
        }
        if (chordDrag) {
            if (!rightDown) chordDrop();  // otherwise the right holds it (tilting, maybe)
            return;
        }
        if (aimHeld && !aimHand && !aimRight) {
            aimHeld = false;
            clickPress = true;  // after this frame's pose, so it lands where the pointer was moved to
            gazeBack = nudgeMoved < 0.2;
            return;
        }
        if (leftHeld) releaseLeft();
    };
    // The right button (see the top); false: not taken here (a tilt, or passed on as it is).
    auto rightButton = [&](bool down) {
        rightDown = down;
        if (down) {
            if (aimHeld && !aimHand && !aimRight) {
                // During the left's held-back press: press the left where the pointer is now (the
                // correction is a lesson), and the mouse drags.
                aimHeld = false;
                chordDrag = gazeBack = true;
                pressLeft();
                if (debug) std::printf("mouse drag began (right during left)\n");
                if (debug) std::fflush(stdout);
                return true;
            }
            if (canAim() && !leftHeld) {
                aimStart(true);
                return true;
            }
            return false;
        }
        if (chordDrag) {
            if (leftDown) return !swallowedRight;  // the left holds it; a tilt's release ends the tilt
            tilting = swallowedRight = false;
            chordDrop();
            return true;
        }
        if (aimHeld && !aimHand && aimRight) {
            aimHeld = aimRight = false;
            pressRight = clickPress = true;  // the right click, where the pointer was moved to
            gazeBack = nudgeMoved < 0.2;
            return true;
        }
        if (leftHeld && heldButton == "b") {  // its press, held still into a real one
            releaseLeft();
            return true;
        }
        return false;
    };
    // POINTER_GAZE_MOUSE_MOVE=held (see the top): the gaze is fresh and nothing is pressed, so a
    // mouse move doesn't move the pointer.
    auto mouseMoveHeld = [&] {
        return gazeOn && cfg.gazeMouseHeld && !inGame && !headsetOff && Clock::now() - gz.at < std::chrono::seconds(1) &&
               !aimHeld && !leftHeld && !tilting && hold.src == Src::None && !clickPress && !clickRelease;
    };

    // --- Panel placement (see "Placement" at the top of the file) ---
    // Device pose, given in the standing universe, sent to the driver in raw space.
    auto sendPose = [&](Vec3 originStanding, const Basis &b) {
        vr::TrackedDevicePose_t s, r;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &s, 1);
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0, &r, 1);
        const auto &S = s.mDeviceToAbsoluteTracking, &R = r.mDeviceToAbsoluteTracking;
        auto toRaw = [&](Vec3 v) { return Rotate(R, RotateInverse(S, v)); };
        const Vec3 o = Position(R) + toRaw(originStanding - Position(S));
        double q[4];
        BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
        char msg[200];
        std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", o.x, o.y, o.z, q[0], q[1], q[2], q[3]);
        SendTo(out, "ft_pointer", msg);
    };
    auto sleepMs = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };
    auto headPos = [&] {
        vr::TrackedDevicePose_t s;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &s, 1);
        return std::make_pair(s.bPoseIsValid, Position(s.mDeviceToAbsoluteTracking));
    };
    // Borrow the device and the laser for a placement: connect, claim, laser mode on.
    auto borrow = [&] {
        SendTo(out, "ft_pointer", "show");
        overlay->ShowOverlay(laserMode);
        overlay->HideOverlay(cursor);
        overlay->HideOverlay(marker);
        sleepMs(active ? 50 : 400);  // a fresh connect needs SteamVR to bind the device
        SendTo(out, "ft_pointer", "btn a 1");
        sleepMs(60);
        SendTo(out, "ft_pointer", "btn a 0");
    };
    auto giveBack = [&] {
        if (!active) {
            SendTo(out, "ft_pointer", "hide");
            overlay->HideOverlay(laserMode);
            laserModeShown = false;
        }
    };
    auto findPanel = [&](const char *key, Panel &p, Vec3 &eye) -> std::string {
        vr::VROverlayHandle_t h;
        if (overlay->FindOverlay(key, &h) != vr::VROverlayError_None) return std::string("no overlay ") + key;
        bool valid;
        std::tie(valid, eye) = headPos();
        if (!valid) return "no head pose (headset off?)";
        p = ScanPanel(h, eye, 0.5);
        if (!p.found) return std::string("panel not visible: ") + key;
        return "";
    };

    // grabprobe (maintenance): aim down from the panel's bottom edge, 1 cm a step, and log where
    // SteamVR's laser hits something (the hit dot shows) and whether the window controls are up.
    auto grabProbe = [&](const char *key) {
        Panel p;
        Vec3 eye;
        const std::string err = findPanel(key, p, eye);
        if (!err.empty()) {
            std::printf("grabprobe: %s\n", err.c_str());
            std::fflush(stdout);
            return;
        }
        borrow();
        vr::VROverlayHandle_t footer = vr::k_ulOverlayHandleInvalid;
        overlay->FindOverlay("valve.steam.gamepadui.floatingfooter", &footer);
        const Vec3 bottom = p.center - p.basis.y * (p.height / 2);
        std::printf("grabprobe %s: center (%.3f %.3f %.3f) %.3f x %.3f m\n", key, p.center.x, p.center.y, p.center.z,
                    p.width, p.height);
        for (int cm = -5; cm <= 45; ++cm) {
            const Vec3 target = bottom - p.basis.y * (cm / 100.0);
            sendPose(eye, AimBasis(target - eye));
            sleepMs(90);
            const bool dot = systemPointer != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(systemPointer);
            const bool foot = footer != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(footer);
            std::printf("  %+3d cm below the bottom edge: steamvr_dot=%d footer=%d", cm, dot, foot);
            if (foot) {
                vr::ETrackingUniverseOrigin uo;
                vr::HmdMatrix34_t t{};
                if (overlay->GetOverlayTransformAbsolute(footer, &uo, &t) == vr::VROverlayError_None) {
                    const Vec3 f = Position(t) - p.center;
                    std::printf(" footer at panel (%.3f %.3f %.3f)", Dot(f, p.basis.x), Dot(f, p.basis.y),
                                Dot(f, p.basis.z));
                }
            }
            std::printf("\n");
        }
        std::fflush(stdout);
        giveBack();
    };

    // Carry a floating panel so its centre lands on `target` with frame `bt` (see
    // "Placement" at the top). Returns "ok ..." or "error ...".
    auto place = [&](const char *key, Vec3 target, const Basis &bt, double grabBelow) -> std::string {
        Panel p;
        Vec3 eye;
        std::string err = findPanel(key, p, eye);
        if (!err.empty()) return "error " + err;
        auto offBy = [&](const Panel &q, double &cm, double &deg) {
            cm = Length(q.center - target) * 100;
            const double c = (Dot(q.basis.x, bt.x) + Dot(q.basis.y, bt.y) + Dot(q.basis.z, bt.z) - 1) / 2;
            deg = std::acos(std::clamp(c, -1.0, 1.0)) * 180 / M_PI;
        };
        double cm, deg;
        int moves = 0;
        borrow();
        for (int attempt = 0; attempt < 3; ++attempt) {
            offBy(p, cm, deg);
            if (cm < 1.5 && deg < 1.0) break;
            // The rigid motion that takes the panel to the target: rotate by R, then move.
            auto turn = [&](Vec3 v) { return FromBasis(bt, ToBasis(p.basis, v)); };
            double q[4];
            BasisQuat({turn({1, 0, 0}), turn({0, 1, 0}), turn({0, 0, 1})}, q);
            const double angle = 2 * std::acos(std::clamp(q[0], -1.0, 1.0));
            const Vec3 axis = std::sin(angle / 2) > 1e-6 ? Normalize({q[1], q[2], q[3]}) : Vec3{0, 1, 0};
            const Vec3 grab = p.center - p.basis.y * (p.height / 2 + grabBelow);
            const Basis d0 = AimBasis(grab - eye);
            const Vec3 o1 = target + turn(eye - p.center);  // device origin at the end
            ++moves;
            sendPose(eye, d0);
            sleepMs(150);  // hover: the window controls come up
            SendTo(out, "ft_pointer", "btn trigger 1");
            sleepMs(150);
            // 1. Rotate about the device origin (the eye): the dashboard applies it exactly.
            const int rsteps = std::max(4, int(angle * 180 / M_PI / kPlaceDegPerSec * 60));
            for (int i = 1; i <= rsteps; ++i) {
                const double a = angle * i / rsteps;
                auto r = [&](Vec3 v) { return RotateAbout(v, axis, a); };
                sendPose(eye, {r(d0.x), r(d0.y), r(d0.z)});
                sleepMs(16);
            }
            // 2. Slide the device slowly: fast moves are accelerated by the dashboard.
            const Basis d1{turn(d0.x), turn(d0.y), turn(d0.z)};
            const int tsteps = std::max(4, int(Length(o1 - eye) / cfg.slideSpeed * 60));
            for (int i = 1; i <= tsteps; ++i) {
                sendPose(eye + (o1 - eye) * (double(i) / tsteps), d1);
                sleepMs(16);
            }
            sleepMs(100);
            SendTo(out, "ft_pointer", "btn trigger 0");
            sleepMs(600);  // the dashboard re-reads the pose up to 150 ms after the release
            err = findPanel(key, p, eye);
            if (!err.empty()) break;
        }
        giveBack();
        if (!err.empty()) return "error after the move: " + err;
        offBy(p, cm, deg);
        char msg[160];
        std::snprintf(msg, sizeof msg, "ok %s off by %.1f cm, %.1f deg after %d move%s", key, cm, deg, moves,
                      moves == 1 ? "" : "s");
        std::printf("place: %s\n", msg);
        std::fflush(stdout);
        return msg;
    };

    while (true) {
        // The headset off: SteamVR drops the HMD's activity to idle as soon as it comes off.
        // An awake pointer (a connected controller, SteamVR's laser mode forced on) kept the
        // displays from sleeping, so it's released at once, and the mouse can't wake it until
        // the headset is back on (then the first mouse input does).
        const auto level = sys->GetTrackedDeviceActivityLevel(vr::k_unTrackedDeviceIndex_Hmd);
        headsetOff = level == vr::k_EDeviceActivityLevel_Idle || level == vr::k_EDeviceActivityLevel_Standby ||
                     level == vr::k_EDeviceActivityLevel_Idle_Timeout;
        if (headsetOff && active) {
            active = false;
            claimPending = claimHeld = false;
            overlay->HideOverlay(cursor);
            overlay->HideOverlay(marker);
            SendTo(out, "ft_pointer", "btn a 0");
            SendTo(out, "ft_pointer", "hide");
            std::printf("headset off: pointer released\n");
            std::fflush(stdout);
        }

        // Outside games, gaze mode keeps the pointer (see the top); the relay needs to know.
        {
            const auto t = Clock::now();
            if (t - inGameAt > std::chrono::milliseconds(500)) {
                inGameAt = t;
                inGame = vr::VRApplications()->GetCurrentSceneProcessId() != 0;
            }
            // Hand gestures in the last two minutes keep it the same way.
            const bool handsRecent = cfg.handsOn && handUsed != Clock::time_point{} && t - handUsed < std::chrono::minutes(2);
            const bool awake = (gazeOn || handsRecent) && !inGame && !headsetOff;
            if (awake != gazeAwake || t - gazeAwakeAt > std::chrono::seconds(5)) {
                if (awake != gazeAwake)
                    std::printf("%s keeps the pointer: %s\n", gazeOn ? "gaze" : "hand use",
                                awake ? "yes" : "no (off, in a game, or headset off)");
                if (awake != gazeAwake) std::fflush(stdout);
                gazeAwake = awake;
                gazeAwakeAt = t;
                SendTo(out, "frametop_relay", awake ? "gazeawake 1" : "gazeawake 0");
            }
        }

        // Commands from the relay.
        char buf[256];
        ssize_t n;
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        while ((n = recvfrom(in, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[n] = 0;
            // Reply to the sender (the layout tool binds an abstract address to get answers).
            const sockaddr_un sender = from;
            const socklen_t senderLen = fromLen;
            fromLen = sizeof from;
            auto reply = [&](const std::string &msg) {
                if (senderLen > offsetof(sockaddr_un, sun_path))
                    sendto(out, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr *>(&sender), senderLen);
            };
            // The gaze, from ft-gazed: not mouse input, it never wakes the pointer.
            double g[4];
            if (std::sscanf(buf, "gz %lf %lf %lf %lf", &g[0], &g[1], &g[2], &g[3]) == 4) {
                gz = {g[0], g[1], g[2], g[3], Clock::now()};
                continue;
            }
            // The gaze calibration panel (see the top): presses answer it instead of clicking.
            {
                int on;
                if (std::sscanf(buf, "calpanel %d", &on) == 1) {
                    const bool was = Clock::now() < calPanelUntil;
                    calPanelUntil = on ? Clock::now() + std::chrono::seconds(3) : Clock::time_point{};
                    if (on && !was) calOpened = true;
                    continue;
                }
            }
            if (Clock::now() < calPanelUntil) {
                const bool accept = !std::strncmp(buf, "btn trigger 1", 13) || !std::strncmp(buf, "gazekey left 1", 14);
                const bool quit = !std::strncmp(buf, "btn b 1", 7) || !std::strncmp(buf, "gazekey right 1", 15);
                if (accept || quit) {
                    SendTo(out, "ft_gazed", accept ? "calaccept" : "calquit");
                    continue;
                }
                if (!std::strncmp(buf, "btn ", 4) || !std::strncmp(buf, "gazekey ", 8) ||
                    !std::strncmp(buf, "precision ", 10) || !std::strncmp(buf, "gazedrag ", 9))
                    continue;  // their releases, and the other buttons: nothing to click now
            }
            {
                char kind[16], source[16];
                int v;
                if (std::sscanf(buf, "%15s %15s %d", kind, source, &v) == 3 &&
                    (!std::strcmp(kind, "precision") || !std::strcmp(kind, "gazedrag"))) {
                    lastMouse = Clock::now();
                    if (!active) wake(Clock::now());
                    devicePresses.push_back({source, !std::strcmp(kind, "gazedrag"), v != 0});
                    continue;
                }
                if (std::sscanf(buf, "gazekey %15s %d", source, &v) == 2 &&
                    (!std::strcmp(source, "left") || !std::strcmp(source, "right"))) {
                    lastMouse = Clock::now();
                    if (!active) wake(Clock::now());
                    keyPresses.push_back({!std::strcmp(source, "right"), v != 0});
                    continue;
                }
            }
            if (std::strcmp(buf, "typing") == 0) {  // not mouse input: it never wakes the pointer
                lastTyping = Clock::now();
                continue;
            }
            if (std::strncmp(buf, "vrbind", 6) == 0) {
                std::printf("controller buttons: %s\n", controllerButtons.Bind(buf + 6).c_str());
                std::fflush(stdout);
                continue;
            }
            if (std::strncmp(buf, "vrglobal", 8) == 0) {
                const char *arg = buf + 8;
                while (*arg == ' ') ++arg;
                ControllerButtons::SetGlobal(std::strncmp(arg, "off", 3) != 0);
                reply(controllerButtons.Status());
                continue;
            }
            if (std::strncmp(buf, "vrstatus", 8) == 0) {
                reply(controllerButtons.Status());
                continue;
            }
            if (std::strncmp(buf, "overlays", 8) == 0) {
                overlays.Request(sender, senderLen);  // answered from the list's thread
                continue;
            }
            if (std::strncmp(buf, "gaze", 4) == 0) {
                const char *arg = buf + 4;
                while (*arg == ' ') ++arg;
                if (*arg != '?') {  // "gaze ?" only asks
                    gazeOn = std::strncmp(arg, "on", 2) == 0    ? true
                             : std::strncmp(arg, "off", 3) == 0 ? false
                                                                : !gazeOn;
                    gazeOwns = true, nudging = false;
                    std::printf("gaze mode %s\n", gazeOn ? "on" : "off");
                    std::fflush(stdout);
                }
                reply(gazeOn ? "ok on" : "ok off");
                continue;
            }
            const bool mouseInput = std::strncmp(buf, "move", 4) == 0 || std::strncmp(buf, "btn", 3) == 0 ||
                                    std::strncmp(buf, "scroll", 6) == 0;
            // A move held back (POINTER_GAZE_MOUSE_MOVE=held) only wakes the pointer: it isn't using
            // the mouse, so a drifting mouse doesn't keep the gaze from taking the pointer back.
            const bool moveHeldBack = std::strncmp(buf, "move", 4) == 0 && mouseMoveHeld();
            if (mouseInput && !moveHeldBack) lastMouse = Clock::now();
            // Any mouse input wakes the pointer (after a controller took over, or a helper restart).
            if (!active && mouseInput) wake(Clock::now());
            if (moveHeldBack) continue;
            double a, b;
            char key[128];
            double px, py, pz, pyaw, ppitch, proll = 0, pgrab = -1;
            if (std::sscanf(buf, "grabprobe %127s", key) == 1) {
                grabProbe(key);
                continue;
            }
            if (std::sscanf(buf, "place %127s %lf %lf %lf %lf %lf %lf %lf", key, &px, &py, &pz, &pyaw, &ppitch, &proll,
                            &pgrab) >= 6) {
                reply(place(key, {px, py, pz}, PanelBasis(pyaw, ppitch, proll), pgrab >= 0 ? pgrab : cfg.grabOffset));
                continue;
            }
            if (std::sscanf(buf, "measure %127s", key) == 1) {
                Panel p;
                Vec3 eye;
                const std::string err = findPanel(key, p, eye);
                char msg[400] = "";
                if (err.empty())
                    std::snprintf(msg, sizeof msg, "ok %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                                  p.center.x, p.center.y, p.center.z, p.width, p.height, p.basis.x.x, p.basis.x.y,
                                  p.basis.x.z, p.basis.y.x, p.basis.y.y, p.basis.y.z, p.basis.z.x, p.basis.z.y,
                                  p.basis.z.z);
                reply(err.empty() ? msg : "error " + err);
                continue;
            }
            if (std::strncmp(buf, "head", 4) == 0) {
                vr::TrackedDevicePose_t h;
                sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &h, 1);
                const Vec3 e = Position(h.mDeviceToAbsoluteTracking);
                const Vec3 f = Rotate(h.mDeviceToAbsoluteTracking, {0, 0, -1});
                char msg[200];
                std::snprintf(msg, sizeof msg, "ok %.4f %.4f %.4f %.2f %.2f", e.x, e.y, e.z,
                              std::atan2(-f.x, -f.z) * 180 / M_PI, std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI);
                reply(h.bPoseIsValid ? msg : "error no head pose (headset off?)");
                continue;
            }
            if (std::strncmp(buf, "debug", 5) == 0) {
                debug = !debug;
                std::printf("debug %s\n", debug ? "on" : "off");
                std::fflush(stdout);
                continue;
            }
            if (std::strncmp(buf, "btn trigger 1", 13) == 0) {
                leftButton(true);
                continue;
            } else if (std::strncmp(buf, "btn trigger 0", 13) == 0) {
                leftButton(false);
                continue;
            } else if (std::strncmp(buf, "btn b 1", 7) == 0 && rightButton(true)) {
                continue;
            } else if (std::strncmp(buf, "btn b 0", 7) == 0 && rightButton(false)) {
                continue;
            } else if (std::strncmp(buf, "btn b 1", 7) == 0 && leftHeld) {
                tilting = tiltStart = swallowedRight = true;  // right press while dragging: tilt, no right-click
                continue;
            } else if (std::strncmp(buf, "btn b 0", 7) == 0 && swallowedRight) {
                tilting = swallowedRight = false;
                continue;
            }
            if (tilting && std::sscanf(buf, "move %lf %lf", &a, &b) == 2) {
                tiltYaw += a;
                tiltPitch = std::clamp(tiltPitch + b, -80.0, 80.0);
                continue;
            }
            if (std::sscanf(buf, "move %lf %lf", &a, &b) == 2) {
                lastMove = Clock::now();  // the dot shows while the mouse moves it (gaze mode)
                if (gazeOn && gazeOwns) {
                    // The mouse takes the pointer from the gaze, from where the gaze left it.
                    gazeOwns = false;
                    nudging = haveHead && Clock::now() - gz.at < std::chrono::milliseconds(200);
                    nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
                    nudgeAt = Clock::now(), nudgeMoved = 0;
                }
                if (nudging || aimHeld) nudgeMoved += std::hypot(a, b);
                if (!anchored) recenter = true;
                yaw += a;
                while (yaw > 180) yaw -= 360;
                while (yaw < -180) yaw += 360;
                pitch = std::clamp(pitch + b, -85.0, 85.0);
            } else if (std::strncmp(buf, "recenter", 8) == 0) {
                recenter = true;
            } else if (std::strncmp(buf, "reload", 6) == 0) {
                cfg.Load(ReadConfig());
                ApplyConfig();
                lastSlow = Clock::now() - std::chrono::seconds(10);  // apply POINTER_IGNORE now
                std::printf("reloaded: free distance %.2f m, dot %.2f deg, origin %.2f, head follow %s, leash %.0f deg, "
                            "controller pickup %.1fx, %zu ignored\n",
                            cfg.freeDistance, cfg.cursorDeg, cfg.originFraction, follow ? "on" : "off", cfg.leashDeg, cfg.pickupScale,
                            cfg.ignore.size());
                std::fflush(stdout);
            } else if (std::strncmp(buf, "follow", 6) == 0) {
                const char *arg = buf + 6;
                while (*arg == ' ') ++arg;
                const bool was = follow;
                follow = std::strncmp(arg, "on", 2) == 0 ? true : std::strncmp(arg, "off", 3) == 0 ? false : !follow;
                if (follow && !was) followReset = true;
                std::printf("head follow %s (leash %.0f deg)\n", follow ? "on" : "off", cfg.leashDeg);
                std::fflush(stdout);
            } else if (std::strncmp(buf, "show", 4) == 0) {
                if (!active) wake(Clock::now());
            } else if (std::strncmp(buf, "hide", 4) == 0) {
                active = false;
                overlay->HideOverlay(cursor);
                overlay->HideOverlay(marker);
                SendTo(out, "ft_pointer", "hide");
            } else {
                SendTo(out, "ft_pointer", buf);  // btn, scroll
            }
        }

        vr::TrackedDevicePose_t all[vr::k_unMaxTrackedDeviceCount];
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.011f, all, vr::k_unMaxTrackedDeviceCount);
        const vr::TrackedDevicePose_t &hmd = all[0];
        vr::TrackedDevicePose_t hmdRaw;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.011f, &hmdRaw, 1);
        const auto tnow = Clock::now();

        // Claim pulse (switchlaserhand on the driver's "a" button, no click).
        if (claimPending && tnow >= claimAt) {
            SendTo(out, "ft_pointer", "btn a 1");
            claimPending = false;
            claimHeld = true;
            claimRelease = tnow + std::chrono::milliseconds(60);
        } else if (claimHeld && tnow >= claimRelease) {
            SendTo(out, "ft_pointer", "btn a 0");
            claimHeld = false;
        }

        // The overlay list is only needed while the pointer is awake (see OverlayList).
        overlays.SetPaused(!active || headsetOff);

        // Laser mode on while the pointer is awake.
        if (active != laserModeShown) {
            laserModeShown = active;
            if (active) overlay->ShowOverlay(laserMode);
            else overlay->HideOverlay(laserMode);
            if (debug) std::printf("laser mode %s\n", active ? "forced on" : "released");
            if (debug) std::fflush(stdout);
        }

        // Didn't get the hand role (a held controller keeps it): release, back off.
        if (active && tnow - wokeAt > std::chrono::seconds(1) && ours != vr::k_unTrackedDeviceIndexInvalid &&
            sys->GetControllerRoleForTrackedDeviceIndex(ours) == vr::TrackedControllerRole_Invalid) {
            active = false;
            claimPending = claimHeld = false;
            overlay->HideOverlay(cursor);
            overlay->HideOverlay(marker);
            SendTo(out, "ft_pointer", "btn a 0");
            SendTo(out, "ft_pointer", "hide");
            noWakeUntil = tnow + std::chrono::seconds(2);
            std::printf("no hand role (a controller is in use): pointer released\n");
            std::fflush(stdout);
        }

        // Last used wins: a real controller being moved releases the pointer (see the top),
        // in gaze mode too.
        if (active && hold.src == Src::None && tnow - lastMouse > std::chrono::milliseconds(500)) {
            for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                if (i == ours || !all[i].bPoseIsValid || all[i].eTrackingResult != vr::TrackingResult_Running_OK ||
                    sys->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) {
                    movingSince[i] = {};
                    continue;
                }
                const auto &v = all[i].vVelocity.v, &w = all[i].vAngularVelocity.v;
                const double speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const double spin = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
                if (speed <= 0.35 * cfg.pickupScale && spin <= 2.0 * cfg.pickupScale) {
                    movingSince[i] = {};
                    continue;
                }
                if (movingSince[i] == Clock::time_point{}) movingSince[i] = tnow;
                if (tnow - movingSince[i] >= std::chrono::milliseconds(100)) {
                    active = false;
                    claimPending = claimHeld = false;
                    overlay->HideOverlay(cursor);
                    overlay->HideOverlay(marker);
                    SendTo(out, "ft_pointer", "btn a 0");
                    SendTo(out, "ft_pointer", "hide");
                    std::printf("controller %u moved (%.2f m/s, %.1f rad/s): pointer released\n", i, speed, spin);
                    std::fflush(stdout);
                    break;
                }
            }
        } else {
            std::fill(std::begin(movingSince), std::end(movingSince), Clock::time_point{});
        }
        const auto &hm = hmd.mDeviceToAbsoluteTracking.m;
        const Vec3 eye{hm[0][3], hm[1][3], hm[2][3]};

        if (recenter && hmd.bPoseIsValid) {
            anchor = eye;
            const Vec3 f{-hm[0][2], -hm[1][2], -hm[2][2]};
            yaw = std::atan2(-f.x, -f.z) * 180 / M_PI;
            pitch = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            anchored = true;
            recenter = false;
            followReset = true;
        }

        // Gaze mode (see the top): the gaze has the pointer, or takes it back when you look
        // well away from it. Not while a press holds the pointer, and only on fresh gaze.
        if (hmd.bPoseIsValid) lastHead = hmd.mDeviceToAbsoluteTracking, haveHead = true;
        if (gazeOn && active && hmd.bPoseIsValid && !tilting && !leftHeld && !aimHeld && !clickPress && !clickRelease &&
            tnow >= dropHoldUntil &&
            tnow - gz.at < std::chrono::milliseconds(150)) {
            const Vec3 g = Rotate(hmd.mDeviceToAbsoluteTracking, Direction(gz.hy, gz.hp));
            if (!gazeOwns && havePoint) {
                const double off = std::acos(std::clamp(Dot(g, Normalize(lastPoint - eye)), -1.0, 1.0)) * 180 / M_PI;
                if (off > cfg.gazeRetake && tnow - lastMouse > std::chrono::milliseconds(300)) {
                    if (retakeSince == Clock::time_point{}) retakeSince = tnow;
                    if (tnow - retakeSince >= std::chrono::milliseconds(120)) gazeOwns = true, nudging = false;
                } else {
                    retakeSince = {};
                }
            }
            if (gazeOwns) {
                retakeSince = {};
                anchor = eye;
                anchored = true;
                yaw = std::atan2(-g.x, -g.z) * 180 / M_PI;
                pitch = std::clamp(std::asin(std::clamp(g.y, -1.0, 1.0)) * 180 / M_PI, -85.0, 85.0);
            }
        }

        // Hands (see the top): pinches and grips from ft-hands.
        {
            vr::TrackedDevicePose_t h0;
            sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &h0, 1);
            if (h0.bPoseIsValid) poses.Add(tnow, h0.mDeviceToAbsoluteTracking);
        }
        // Where a gesture's point is, seen from the hold's origin: yaw and pitch, degrees.
        auto handAngles = [&](const float p[3], uint64_t t_ns, const Vec3 &from, double &hy, double &hp) {
            vr::HmdMatrix34_t head;
            if (!poses.At(t_ns, head)) return false;
            const Vec3 d = Normalize(Position(head) + Rotate(head, Vec3{p[0], p[1], p[2]}) - from);
            hy = std::atan2(-d.x, -d.z) * 180 / M_PI;
            hp = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI;
            return true;
        };
        auto endHold = [&](bool lost) {
            if (hold.pressed) {
                if (leftHeld) releaseLeft();
            } else if (aimHeld) {
                aimHeld = aimHand = false;
                if (lost) {
                    if (gazeOn) gazeOwns = true, nudging = false;  // no click: the pointer goes back to the gaze
                } else {
                    clickPress = true;  // the click is on the release, where the pointer is now
                    pressRight = hold.right;
                    gazeBack = nudgeMoved < 0.2;
                }
            }
            if (debug)
                std::printf("hold %s %s%s\n", hold.src == Src::Hand ? (hold.grip ? "grip" : "pinch")
                                              : hold.src == Src::Head ? (hold.right ? "key right" : "key left")
                                              : hold.grip ? "gaze drag" : "gaze precision",
                            lost ? "lost" : "released", hold.pressed ? "" : lost ? " (no click)" : " (click)");
            if (debug) std::fflush(stdout);
            hold = {};
        };
        // The press, once a hold's source is set: a real press (dragging until the release), or
        // held back like gaze mode's mouse press (see the top): the click comes on the release.
        auto startHold = [&](bool press) {
            hold.startYaw = hold.lastYaw = yaw, hold.startPitch = hold.lastPitch = pitch;
            hold.pressed = press;
            if (press) {
                gazeBack = gazeOn && gazeOwns;
                pressLeft();
            } else {
                gazeOwns = false;
                nudging = gazeOn && haveHead && tnow - gz.at < std::chrono::milliseconds(200);
                nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
                nudgeAt = aimSince = tnow, nudgeMoved = 0;
                aimHeld = aimHand = true;
                pulseAt = tnow;
            }
        };
        // The hold's source moved to (hy, hp): past the dead zone, the pointer follows at the gain,
        // from where it was when it got past.
        auto steer = [&](double hy, double hp, double deadzone, double gain) {
            const double dy = std::remainder(hy - hold.refYaw, 360.0), dp = hp - hold.refPitch;
            if (!hold.engaged) {
                if (std::hypot(dy, dp) <= deadzone) return;
                hold.engaged = true;
                hold.refYaw = hy, hold.refPitch = hp;
                hold.startYaw = hold.lastYaw = yaw, hold.startPitch = hold.lastPitch = pitch;
                return;
            }
            yaw = std::remainder(hold.startYaw + gain * dy, 360.0);
            pitch = std::clamp(hold.startPitch + gain * dp, -85.0, 85.0);
            // How far the nudge went: for a keyboard click, from where the dot was at the press
            // (a head wobbles on the way); otherwise the path, as with the mouse.
            if (!hold.pressed && hold.src == Src::Head)
                nudgeMoved = std::hypot(std::remainder(yaw - hold.pressYaw, 360.0), pitch - hold.pressPitch);
            else if (!hold.pressed)
                nudgeMoved += std::hypot(std::remainder(yaw - hold.lastYaw, 360.0), pitch - hold.lastPitch);
            hold.lastYaw = yaw, hold.lastPitch = pitch;
            lastMove = tnow;  // the dot shows while it moves (gaze mode)
        };
        auto beginHold = [&](int side, bool grip, const fh_pinch_t &g) {
            handUsed = tnow;
            if (hold.src != Src::None) {
                // A grip takes over a pinch on the way to it (closing the hand passes through
                // one); otherwise one hold at a time.
                if (hold.src != Src::Hand || !grip || hold.grip) return;
                aimHeld = aimHand = false;
                hold = {};
            }
            if (leftHeld || aimHeld || clickPress || clickRelease) return;  // the mouse's button is busy
            if (!active) {
                wake(tnow);  // the first gesture only wakes the pointer, like the first mouse move
                return;
            }
            vr::HmdMatrix34_t head;
            if (!poses.At(g.begin_ns, head)) return;
            // No pinch just after a key (typing touches thumb to index), and a grip only with
            // the hand held up (hands on a desk curl like a loose fist; looking down at them
            // puts them straight ahead in the head's frame, where ft-hands can't tell).
            const Vec3 at = Position(head) + Rotate(head, Vec3{g.begin_point[0], g.begin_point[1], g.begin_point[2]});
            if (!grip && tnow - lastTyping < std::chrono::duration<double>(cfg.typingHold)) {
                if (debug) std::printf("hand pinch ignored: typing\n");
                return;
            }
            if (grip && Position(head).y - at.y > cfg.handBelow) {
                if (debug) std::printf("hand grip ignored: %.2f m below the eyes\n", Position(head).y - at.y);
                return;
            }
            Hold h;
            h.src = Src::Hand, h.side = side, h.grip = grip, h.origin = Position(head);
            if (!handAngles(g.begin_point, g.begin_ns, h.origin, h.refYaw, h.refPitch)) return;
            hold = h;
            // A grip, or a pinch without gaze mode: a real press where the pointer is (where
            // you look, in gaze mode), dragging with the hand until it opens.
            startHold(grip || !gazeOn);
            if (debug) std::printf("hand %s %s began\n", side ? "right" : "left", grip ? "grip" : "pinch");
            if (debug) std::fflush(stdout);
        };
        // Gaze precision and gaze drag buttons (see the top): the mouse steers.
        for (const DevicePress &p : devicePresses) {
            if (p.source == "left" || p.source == "right") continue;  // no controllers in gaze mode
            if (!p.down) {
                if (hold.src == Src::Mouse) endHold(false);
                continue;
            }
            if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease) continue;
            Hold h;
            h.src = Src::Mouse, h.grip = p.drag;
            hold = h;
            startHold(p.drag);
            if (debug) std::printf("hold gaze %s began (%s)\n", p.drag ? "drag" : "precision", p.source.c_str());
            if (debug) std::fflush(stdout);
        }
        devicePresses.clear();
        // Keyboard clicks (see the top): held back at the gaze, steered by the head.
        auto headAngles = [&](double &hy, double &hp) {
            if (!hmd.bPoseIsValid) return false;
            const Vec3 f = Normalize({-hm[0][2], -hm[1][2], -hm[2][2]});
            hy = std::atan2(-f.x, -f.z) * 180 / M_PI;
            hp = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            return true;
        };
        for (const KeyPress &k : keyPresses) {
            (k.right ? keyRightDown : keyLeftDown) = k.down;
            if (!k.down) {
                if (keyTilting && k.right) {  // the tilt ends; the head drags again from here
                    keyTilting = tilting = false;
                    hold.engaged = false;
                    if (debug) std::printf("hold key left: tilt ended\n");
                    if (debug) std::fflush(stdout);
                }
                // The keys holding it: a gaze_left then gaze_right drag, either; otherwise its own.
                const bool held = hold.keyDrag ? keyLeftDown || keyRightDown : hold.right ? keyRightDown : keyLeftDown;
                if (hold.src == Src::Head && !held) {
                    if (!hold.pressed && aimHeld && tnow - aimSince < std::chrono::duration<double>(cfg.keyTap)) {
                        // A quick tap: a click where the dot was at the press, and the gaze was right.
                        yaw = hold.pressYaw, pitch = hold.pressPitch;
                        nudgeMoved = 0;
                        confirmLesson = true;
                    }
                    endHold(false);
                    keyTilting = false;
                }
                continue;
            }
            if (k.right && hold.src == Src::Head && !hold.right && hold.pressed && leftHeld) {
                // gaze_right during a gaze_left drag: tilt while it's held (see the top).
                tilting = tiltStart = keyTilting = true;
                headAngles(keyTiltYaw, keyTiltPitch);
                if (debug) std::printf("hold key left: tilt began\n");
                if (debug) std::fflush(stdout);
                continue;
            }
            if (k.right && hold.src == Src::Head && !hold.right && !hold.pressed && aimHeld) {
                // gaze_right while gaze_left aims: press the left button where the dot is now
                // (the correction is a lesson), then the head drags.
                aimHeld = aimHand = false;
                hold.pressed = hold.grip = hold.keyDrag = true, hold.promote = false, hold.engaged = false;
                headAngles(hold.refYaw, hold.refPitch);
                gazeBack = gazeOn;
                pressRight = false;
                pressLeft();
                if (debug) std::printf("hold key left: pressed (right key)\n");
                if (debug) std::fflush(stdout);
                continue;
            }
            if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease) continue;
            Hold h;
            h.src = Src::Head, h.promote = true, h.right = k.right, h.pressYaw = yaw, h.pressPitch = pitch;
            if (!headAngles(h.refYaw, h.refPitch)) continue;
            hold = h;
            startHold(false);
            if (debug) std::printf("hold key %s began\n", k.right ? "right" : "left");
            if (debug) std::fflush(stdout);
        }
        keyPresses.clear();
        if (calOpened) {  // the calibration panel came up: a press in progress ends, no click
            calOpened = false;
            if (hold.src != Src::None) endHold(true);
            if (leftHeld) releaseLeft();
            aimHeld = aimHand = clickPress = false;
        }
        if (hold.src == Src::Head) {
            double hy, hp;
            if (headAngles(hy, hp) && keyTilting) {
                // The head turns the panel, as the mouse does in a tilt; the pointer stays put.
                tiltYaw += std::remainder(hy - keyTiltYaw, 360.0);
                tiltPitch = std::clamp(tiltPitch + hp - keyTiltPitch, -80.0, 80.0);
                keyTiltYaw = hy, keyTiltPitch = hp;
            } else if (headAngles(hy, hp)) {
                steer(hy, hp, hold.pressed ? 0.3 : cfg.headDeadzone, 1.0);
            }
        }
        fh_gestures_t hg;
        const bool handOk = cfg.handsOn && handFile.Read(hg);
        if (handOk && (hg.seq != handSeq || handFile.opens != handOpens)) {
            handSeq = hg.seq;
            handPublished = hg.publish_ns;
            const fh_pinch_t *slots[2] = {hg.pinch, hg.grip};
            // A new file, or a tracker whose counters went back: start counting from here.
            bool rebase = !handBaseline || handFile.opens != handOpens;
            for (int k = 0; k < 2; ++k)
                for (int s = 0; s < 2; ++s)
                    rebase = rebase || slots[k][s].begins < seenBegins[k][s] || slots[k][s].ends < seenEnds[k][s];
            if (rebase) {
                if (hold.src == Src::Hand) endHold(true);
                for (int k = 0; k < 2; ++k)
                    for (int s = 0; s < 2; ++s) seenBegins[k][s] = slots[k][s].begins, seenEnds[k][s] = slots[k][s].ends;
                handBaseline = true, handOpens = handFile.opens;
            }
            // Not over a VR game (unless the dashboard is up), and not with the headset off.
            const bool allowed = !headsetOff && (!inGame || overlay->IsDashboardVisible());
            for (int k = 1; k >= 0; --k)  // grips first: one takes over a pinch
                for (int s = 0; s < 2; ++s) {
                    const fh_pinch_t &g = slots[k][s];
                    const bool began = g.begins != seenBegins[k][s], ended = g.ends != seenEnds[k][s];
                    const bool lost = g.flags & FH_PINCH_LOST;
                    seenBegins[k][s] = g.begins, seenEnds[k][s] = g.ends;
                    auto holding = [&] { return hold.src == Src::Hand && hold.side == s && hold.grip == (k == 1); };
                    if (holding() && ended) endHold(lost);  // the one held ended (another may have begun)
                    if (began && allowed) {
                        beginHold(s, k == 1, g);
                        // begun and ended since the last read (a quick tap): its release too
                        if (!(g.flags & FH_PINCH_DOWN) && holding()) endHold(lost);
                    }
                }
            // The held gesture's hand moves the pointer: past the dead zone (a tap's jitter,
            // the pinch point shifting as the fingers close), then at the gain, from there.
            if (hold.src == Src::Hand) {
                const fh_pinch_t &g = hold.grip ? hg.grip[hold.side] : hg.pinch[hold.side];
                double hy, hp;
                if ((g.flags & FH_PINCH_TRACKED) && handAngles(g.point, hg.capture_ns, hold.origin, hy, hp))
                    steer(hy, hp, hold.grip ? 0.5 : cfg.pinchDeadzone, hold.grip ? cfg.gripGain : cfg.pinchGain);
            }
        }
        // ft-hands stopped (or hung) with a gesture held: it's over, as lost.
        const uint64_t nowNs =
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow.time_since_epoch()).count());
        if (hold.src == Src::Hand && (!handOk || !active || nowNs - handPublished > 1'500'000'000ull)) endHold(true);
        if (hold.src != Src::None && hold.src != Src::Hand && !active) endHold(true);

        if (aimHeld || leftHeld || clickPress || clickRelease) lastHeld = tnow;

        // Head follow (see the top): past the leash (for the delay), ease the reference to the
        // head's facing, and turn the cursor with it. Not in gaze mode: the gaze places it.
        if (follow && !gazeOn && active && anchored && hmd.bPoseIsValid) {
            const Vec3 head = LimitPitch(Vec3{-hm[0][2], -hm[1][2], -hm[2][2]}, 85);
            if (followReset) {
                followRef = head, followLag = 0, leashOutSince = {};
                followReset = following = false;
            }
            const double dt = std::min(0.1, std::chrono::duration<double>(tnow - followAt).count());
            const double leash = cfg.leashDeg * M_PI / 180;
            const double lag = std::acos(std::clamp(Dot(followRef, head), -1.0, 1.0));
            double keep = lag;  // how far the reference stays behind the head after this frame
            if (leash <= 0) {
                keep = 0;  // head-locked
            } else if (following) {
                keep = lag * (cfg.leashReturn > 0 ? std::exp(-dt / cfg.leashReturn) : 0.0);
                // A fast turn drags it at the leash's end; past it already (after the delay), it
                // can't fall further behind, and it closes in from there without a jump.
                keep = std::min(keep, std::max(leash, followLag));
                if (keep < 0.05 * M_PI / 180) keep = 0, following = false;  // landed on the facing
            } else if (lag > leash) {
                if (leashOutSince == decltype(leashOutSince){}) leashOutSince = tnow;
                if (std::chrono::duration<double>(tnow - leashOutSince).count() >= cfg.leashDelay)
                    following = true, leashOutSince = {};
            } else {
                leashOutSince = {};  // back inside before the delay: a glance
            }
            followLag = keep;
            const Vec3 ref = LimitPitch(PullWithin(followRef, head, keep), 85);
            if (!leftHeld && tnow >= dropHoldUntil) {
                // The cursor keeps its offset from the reference (frames without roll).
                Vec3 d = FromBasis(AimBasis(ref), ToBasis(AimBasis(followRef), Direction(yaw, pitch)));
                d = PullWithin(Normalize(d), ref, cfg.followReach * M_PI / 180);
                yaw = std::atan2(-d.x, -d.z) * 180 / M_PI;
                pitch = std::clamp(std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI, -85.0, 85.0);
                // The ray origin closes in on the eye as the reference does on the facing.
                anchor = eye + (anchor - eye) * (leash <= 0 ? 0.0 : lag > 1e-6 ? keep / lag : following ? 0.0 : 1.0);
            }
            followRef = ref;
        }
        followAt = tnow;

        // Slow work, once a second: overlay handles, our device index, laser width.
        const auto now = std::chrono::steady_clock::now();
        if (now - lastSlow > std::chrono::seconds(1)) {
            lastSlow = now;
            handles.clear();
            for (const auto &key : overlays.Keys()) {
                if (Ignored(cfg.ignore, key)) continue;
                vr::VROverlayHandle_t h;
                if (overlay->FindOverlay(key.c_str(), &h) != vr::VROverlayError_None) continue;
                handles[key] = h;
                // Scene-graph overlays are placed absolutely. Other overlays can report no
                // texture too (gamescope's app panels, placed as dashboard tabs, share theirs
                // from another process), and ComputeOverlayIntersection handles those.
                uint32_t tw = 0, th = 0;
                overlay->GetOverlayTextureSize(h, &tw, &th);
                vr::VROverlayTransformType tt = vr::VROverlayTransform_Invalid;
                overlay->GetOverlayTransformType(h, &tt);
                // ft-screens' panels (frametop.screen.N, floating windows with their popups,
                // frametop.float.N..., the keyboard) are 0x0 and absolute too (a shared
                // texture), but they're real panels of any size.
                sceneGraph[key] = (tw == 0 || th == 0) && tt == vr::VROverlayTransform_Absolute &&
                                  key.rfind("frametop.", 0) != 0;
            }
            ours = vr::k_unTrackedDeviceIndexInvalid;
            for (vr::TrackedDeviceIndex_t i = 0; i < vr::k_unMaxTrackedDeviceCount; ++i) {
                char type[64] = "";
                sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
                if (std::strcmp(type, "ft_pointer") == 0) ours = i;
            }

        }

        if (now - lastVisible > std::chrono::milliseconds(50) || visible.size() != handles.size()) {
            lastVisible = now;
            visible.clear();
            for (const auto &[key, h] : handles) visible[key] = overlay->IsOverlayVisible(h);
        }

        if (active && anchored && hmd.bPoseIsValid && tilting) {
            // Rotate the device around the grab point; the grabbed panel turns with it.
            if (tiltStart) {
                pivot = lastPoint;
                tiltOrigin = lastOrigin;
                tiltBasis = AimBasis(lastAim);
                tiltStart = false;  // angles carry on from any earlier tilt in this drag
                overlay->HideOverlay(cursor);
                overlay->HideOverlay(marker);
            }
            const double yr = tiltYaw * M_PI / 180, pr = tiltPitch * M_PI / 180;
            const Vec3 up{0, 1, 0}, side = tiltBasis.x;
            auto turn = [&](Vec3 v) { return RotateAbout(RotateAbout(v, side, pr), up, yr); };
            const Vec3 originStanding = pivot + turn(tiltOrigin - pivot);
            const Basis b{turn(tiltBasis.x), turn(tiltBasis.y), turn(tiltBasis.z)};
            const auto &S = hmd.mDeviceToAbsoluteTracking, &R = hmdRaw.mDeviceToAbsoluteTracking;
            auto toRaw = [&](Vec3 v) { return Rotate(R, RotateInverse(S, v)); };  // directions: raw <- standing
            const Vec3 originRaw = Position(R) + toRaw(originStanding - eye);
            double q[4];
            BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
            char msg[200];
            std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", originRaw.x, originRaw.y,
                          originRaw.z, q[0], q[1], q[2], q[3]);
            SendTo(out, "ft_pointer", msg);
        } else if (active && anchored && hmd.bPoseIsValid) {
            const bool dragging = leftHeld || tnow < dropHoldUntil;
            if (!dragging) tiltYaw = tiltPitch = 0;  // drop finished: back to plain pointing
            const Vec3 dir = Direction(yaw, pitch);
            // Nearest visible overlay along a ray (frozen while dragging).
            struct Hit {
                double along = 1e9;
                std::string key;
                bool scene = false;
                Vec3 point, normal;
            };
            auto nearest = [&](Vec3 from, Vec3 d) {
                Hit h;
                for (const auto &[key, handle] : handles) {
                    if (!visible[key]) continue;
                    if (sceneGraph[key]) {
                        // Plane test: overlay origin and its +Z normal, within sceneRadius of the origin.
                        vr::ETrackingUniverseOrigin uo;
                        vr::HmdMatrix34_t t{};
                        if (overlay->GetOverlayTransformAbsolute(handle, &uo, &t) != vr::VROverlayError_None) continue;
                        const Vec3 center = Position(t), normal{t.m[0][2], t.m[1][2], t.m[2][2]};
                        const double denom = Dot(d, normal);
                        if (std::fabs(denom) < 1e-4) continue;
                        const double along = Dot(center - from, normal) / denom;
                        const Vec3 at = from + d * along;
                        if (along > 0.05 && along < h.along && std::sqrt(Dot(at - center, at - center)) <= cfg.sceneRadius)
                            h.along = along, h.key = key, h.scene = true, h.point = at, h.normal = normal;
                        continue;
                    }
                    vr::VROverlayIntersectionParams_t params{};
                    params.vSource = {float(from.x), float(from.y), float(from.z)};
                    params.vDirection = {float(d.x), float(d.y), float(d.z)};
                    params.eOrigin = vr::TrackingUniverseStanding;
                    vr::VROverlayIntersectionResults_t r{};
                    if (overlay->ComputeOverlayIntersection(handle, &params, &r) && r.fDistance > 0.05f &&
                        r.fDistance < h.along) {
                        h.along = r.fDistance, h.key = key, h.scene = false;
                        h.point = {r.vPoint.v[0], r.vPoint.v[1], r.vPoint.v[2]};
                        h.normal = {r.vNormal.v[0], r.vNormal.v[1], r.vNormal.v[2]};
                    }
                }
                return h;
            };
            Hit first;
            if (!dragging) first = nearest(anchor, dir);
            double best = first.along;
            std::string bestKey = first.key;
            bool bestScene = first.scene;
            Vec3 bestPoint = first.point, bestNormal = first.normal;
            bool onEdge = false;
            if (!dragging && best < 1e8 && !bestScene) {
                edgeKey = bestKey, edgePoint = bestPoint, edgeNormal = Normalize(bestNormal), edgeLast = bestPoint;
            } else if (!dragging && best >= 1e8 && !edgeKey.empty() && visible[edgeKey]) {
                // Just off a panel: stay on its plane (see "Panel edges" at the top).
                const double denom = Dot(dir, edgeNormal);
                if (std::fabs(denom) > 1e-4) {
                    const double along = Dot(edgePoint - anchor, edgeNormal) / denom;
                    const Vec3 at = anchor + dir * along;
                    if (along > 0.05 && std::sqrt(Dot(at - edgeLast, at - edgeLast)) <= cfg.edgeReach) {
                        best = along;
                        bestKey = edgeKey;
                        onEdge = true;
                    }
                }
            }
            // Held on one of ft-screens' panels that stays where it is: onto whichever of them
            // the ray meets (see the top).
            if (leftHeld && !pressKey.empty()) {
                vr::ETrackingUniverseOrigin uo;
                vr::HmdMatrix34_t now{};
                auto it = handles.find(pressKey);
                bool still = it != handles.end() &&
                             overlay->GetOverlayTransformAbsolute(it->second, &uo, &now) == vr::VROverlayError_None;
                for (int i = 0; still && i < 3; ++i)
                    for (int j = 0; j < 4; ++j)
                        if (std::fabs(now.m[i][j] - pressPose.m[i][j]) > 0.001f) still = false;
                if (still) {
                    Hit h;
                    for (const auto &[key, handle] : handles) {
                        if (!visible[key] || !FramePanel(key)) continue;
                        vr::VROverlayIntersectionParams_t params{};
                        params.vSource = {float(anchor.x), float(anchor.y), float(anchor.z)};
                        params.vDirection = {float(dir.x), float(dir.y), float(dir.z)};
                        params.eOrigin = vr::TrackingUniverseStanding;
                        vr::VROverlayIntersectionResults_t r{};
                        if (overlay->ComputeOverlayIntersection(handle, &params, &r) && r.fDistance > 0.05f &&
                            r.fDistance < h.along)
                            h.along = r.fDistance, h.key = key;
                    }
                    if (h.along < 1e8) dragDistance = h.along, lastHit = h.key;
                } else {
                    pressKey.clear();  // carried: the lock holds for the rest of this press
                }
            }
            // While dragging: keep the press-time distance and show the non-interactive marker.
            // On a scene-graph plane or a panel's edge: the laser-catching dot goes 5 cm behind it.
            double distance = dragging ? dragDistance : (best < 1e8 ? best : cfg.freeDistance);
            Vec3 point = anchor + dir * distance;
            bool occluded = false;
            if (!dragging) {
                // The cursor lands on what you see under it: the ray above starts at the anchor,
                // not the eye, so after leaning it can pick a panel that something nearer
                // covers from where you are now (panels close together in view, at different
                // depths). Anything in front of the point on the eye's line of sight wins.
                const double toPoint = std::sqrt(Dot(point - eye, point - eye));
                const Hit front = nearest(eye, Normalize(point - eye));
                if (front.along < toPoint - 0.02) {
                    occluded = true;
                    bestKey = front.key, bestScene = front.scene;
                    point = front.point;
                    if (!front.scene) edgeKey = front.key, edgePoint = front.point, edgeNormal = Normalize(front.normal),
                                      edgeLast = front.point;
                    distance = std::sqrt(Dot(point - anchor, point - anchor));
                    best = distance;
                    onEdge = false;
                }
                lastDistance = distance, lastHit = bestKey;
            }
            const bool onScene = !dragging && ((bestScene && best < 1e8) || onEdge);
            const bool onPanel = dragging || (best < 1e8 && !onScene);
            const Vec3 sight = Normalize(point - eye);
            if (!dragging) {
                // SteamVR Settings (see the top): the dashboard's main panel is hidden and its
                // scene-graph panel shows the page.
                onVrSettings = false;
                const auto mainIt = visible.find("valve.steam.gamepadui.main");
                if (overlay->IsDashboardVisible() && !(mainIt != visible.end() && mainIt->second)) {
                    for (const auto &[key, handle] : handles) {
                        if (!visible[key] || key.rfind("valve.steam.gamepadui.frame.menu.", 0) != 0) continue;
                        vr::ETrackingUniverseOrigin uo;
                        vr::HmdMatrix34_t t{};
                        if (overlay->GetOverlayTransformAbsolute(handle, &uo, &t) == vr::VROverlayError_None &&
                            OnSettingsPage(t, eye, sight))
                            onVrSettings = true;
                    }
                }
            }
            if (onVrSettings != catcherHidesHit) {
                overlay->SetOverlayFlag(cursor, vr::VROverlayFlags_HideLaserIntersection, onVrSettings);
                catcherHidesHit = onVrSettings;
            }

            if (onVrSettings) {
                // The dot close in front of the page, which is nearer than any guess of ours;
                // the laser-catching dot far behind everything, invisible, so it never covers
                // the page, with SteamVR's hit dot hidden on it.
                const Vec3 near = eye + sight * SETTINGS_DOT, far = eye + sight * SETTINGS_CATCHER;
                double alpha = 1;
                if (gazeOn && !cfg.gazeDotAlways) {
                    auto secs = [&](Clock::time_point t) { return std::chrono::duration<double>(tnow - t).count(); };
                    alpha = std::clamp(1 - std::min(secs(lastMove) - cfg.gazeShow, secs(lastHeld)) / 0.25, 0.0, 1.0);
                }
                if (tnow < calPanelUntil) alpha = 0;
                overlay->SetOverlayAlpha(marker, float(alpha));
                overlay->SetOverlayWidthInMeters(marker, float(2 * SETTINGS_DOT * std::tan(cfg.cursorDeg * M_PI / 360)));
                auto mm = Billboard(near, eye);
                overlay->SetOverlayTransformAbsolute(marker, vr::TrackingUniverseStanding, &mm);
                overlay->SetOverlayAlpha(cursor, 0);
                overlay->SetOverlayWidthInMeters(cursor, float(2 * SETTINGS_CATCHER * std::tan(cfg.cursorDeg * M_PI / 360)));
                auto mc = Billboard(far, eye);
                overlay->SetOverlayTransformAbsolute(cursor, vr::TrackingUniverseStanding, &mc);
                overlay->ShowOverlay(marker);
                overlay->ShowOverlay(cursor);
            } else {
                // On a panel: the non-interactive marker, pulled 5 mm toward the eye so it
                // draws on top. In free space: the interactive dot the laser lands on.
                const vr::VROverlayHandle_t show = onPanel ? marker : cursor, hide = onPanel ? cursor : marker;
                const Vec3 at = onPanel ? point + Normalize(eye - point) * 0.005 : onScene ? point + dir * 0.05 : point;
                const double dist = std::sqrt(Dot(at - eye, at - eye));
                // Gaze mode: a pulse for each click, and with POINTER_GAZE_DOT=moving, shown only
                // while something moves it or a press holds it (see the top); transparent
                // otherwise, the laser still lands on it.
                double scale = 1, alpha = 1;
                if (gazeOn) {
                    auto secs = [&](Clock::time_point t) { return std::chrono::duration<double>(tnow - t).count(); };
                    alpha = cfg.gazeDotAlways ? 1.0 : std::clamp(1 - std::min(secs(lastMove) - cfg.gazeShow, secs(lastHeld)) / 0.25, 0.0, 1.0);
                    const double pulse = secs(pulseAt);
                    if (pulse < 0.6) {
                        scale = 1 + 1.5 * std::max(0.0, 1 - pulse / 0.3);
                        alpha = std::max(alpha, std::clamp((0.6 - pulse) / 0.3, 0.0, 1.0));
                    }
                }
                if (tnow < calPanelUntil) alpha = 0;  // the calibration panel is up (see the top)
                overlay->SetOverlayAlpha(show, float(alpha));
                overlay->SetOverlayWidthInMeters(show, float(2 * dist * std::tan(scale * cfg.cursorDeg * M_PI / 360)));
                auto m = Billboard(at, eye);
                overlay->SetOverlayTransformAbsolute(show, vr::TrackingUniverseStanding, &m);
                overlay->ShowOverlay(show);
                overlay->HideOverlay(hide);
            }

            // Controller ray: from the eye, aimed at the cursor point, converted from the
            // standing universe to raw tracking space via the HMD's pose in both.
            const Vec3 aimStanding = Normalize(point - eye);
            const auto &S = hmd.mDeviceToAbsoluteTracking, &R = hmdRaw.mDeviceToAbsoluteTracking;
            const Vec3 aim = Rotate(R, RotateInverse(S, aimStanding));  // raw <- head <- standing
            // Origin partway along the line of sight to the cursor (smaller hit dot).
            const double toPoint = std::sqrt(Dot(point - eye, point - eye));
            double originDist = std::max(0.0, std::min(toPoint * cfg.originFraction, toPoint - cfg.originMargin));
            if (onVrSettings) originDist = std::min(originDist, SETTINGS_ORIGIN);  // SteamVR finds the page
            const Vec3 originStanding = eye + Normalize(point - eye) * originDist;
            const Vec3 eyeRaw = Position(R) + Rotate(R, RotateInverse(S, originStanding - eye));
            lastPoint = point, lastOrigin = originStanding, lastAim = aimStanding;  // tilt starts from here
            havePoint = true;
            if (debug && tnow - lastDebug > std::chrono::milliseconds(500)) {
                lastDebug = tnow;
                if (systemPointer == vr::k_ulOverlayHandleInvalid) overlay->FindOverlay("system.pointer", &systemPointer);
                std::printf("dbg %s hit=%s dist=%.2f eye->point=%.2f origin=%.2f vrsettings=%d yaw=%.1f pitch=%.1f gaze=%s steamvr_dot=%d primary=%u\n",
                            dragging ? "DRAG" : occluded ? "INFRONT" : onEdge ? "EDGE" : onScene ? "SCENE" : (best < 1e8 ? "PANEL" : "FREE"), lastHit.empty() ? "-" : lastHit.c_str(),
                            distance, toPoint, originDist, onVrSettings, yaw, pitch,
                            !gazeOn ? "off" : tnow - gz.at > std::chrono::milliseconds(150) ? "stale" : gazeOwns ? "owns" : "mouse",
                            systemPointer != vr::k_ulOverlayHandleInvalid && overlay->IsOverlayVisible(systemPointer),
                            overlay->GetPrimaryDashboardDevice());
                std::fflush(stdout);
            }
            const double ayaw = std::atan2(-aim.x, -aim.z) * 180 / M_PI;
            const double apitch = std::asin(std::clamp(aim.y, -1.0, 1.0)) * 180 / M_PI;
            if (dragging && (tiltYaw != 0 || tiltPitch != 0)) {
                // Keep this drag's tilt applied, about the current cursor point.
                const double yr = tiltYaw * M_PI / 180, pr = tiltPitch * M_PI / 180;
                const Basis base = AimBasis(aimStanding);
                const Vec3 up{0, 1, 0}, side = base.x;
                auto turn = [&](Vec3 v) { return RotateAbout(RotateAbout(v, side, pr), up, yr); };
                const Vec3 o = point + turn(originStanding - point);
                const Basis b{turn(base.x), turn(base.y), turn(base.z)};
                auto toRaw = [&](Vec3 v) { return Rotate(R, RotateInverse(S, v)); };
                const Vec3 oRaw = Position(R) + toRaw(o - eye);
                double q[4];
                BasisQuat({toRaw(b.x), toRaw(b.y), toRaw(b.z)}, q);
                char msg[200];
                std::snprintf(msg, sizeof msg, "posq %.5f %.5f %.5f %.6f %.6f %.6f %.6f", oRaw.x, oRaw.y, oRaw.z, q[0],
                              q[1], q[2], q[3]);
                SendTo(out, "ft_pointer", msg);
            } else {
                char msg[160];
                std::snprintf(msg, sizeof msg, "pose %.5f %.5f %.5f %.4f %.4f", eyeRaw.x, eyeRaw.y, eyeRaw.z, ayaw, apitch);
                SendTo(out, "ft_pointer", msg);
            }
        }

        // A held-back press (see the top): held still long enough, it's a real press (a drag);
        // released, it's a click where the pointer is now (this frame's pose has gone out).
        if (!active) aimHeld = aimRight = clickPress = aimHand = confirmLesson = false;  // released meanwhile: nothing to click
        if (aimHeld && !aimHand && nudgeMoved < 0.2 && tnow - aimSince >= std::chrono::duration<double>(cfg.gazeHold)) {
            aimHeld = false;
            gazeBack = true;
            pressRight = aimRight;  // the right button's: a real right press (see the top)
            aimRight = false;
            pressLeft();
        }
        // A keyboard click held still for POINTER_GAZE_HOLD: a real press, then the head drags
        // (from where it is now).
        if (aimHeld && aimHand && hold.promote && !hold.engaged &&
            tnow - aimSince >= std::chrono::duration<double>(cfg.gazeHold)) {
            aimHeld = aimHand = false;
            hold.pressed = hold.grip = true;
            if (hmd.bPoseIsValid) {
                const Vec3 f = Normalize({-hm[0][2], -hm[1][2], -hm[2][2]});
                hold.refYaw = std::atan2(-f.x, -f.z) * 180 / M_PI;
                hold.refPitch = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            }
            gazeBack = gazeOn;
            pressRight = hold.right;
            pressLeft();
            if (debug) std::printf("hold key %s: pressed (held still)\n", hold.right ? "right" : "left");
            if (debug) std::fflush(stdout);
        }
        if (clickPress) {
            clickPress = false;
            pressLeft();
            clickRelease = true;
            clickReleaseAt = tnow + std::chrono::milliseconds(40);
        } else if (clickRelease && tnow >= clickReleaseAt) {
            clickRelease = false;
            releaseLeft();
        }

        controllerButtons.Poll(
            [&](const char *button, bool down) {
                SendTo(out, "frametop_relay", std::string("vrbtn ") + button + (down ? " 1" : " 0"));
            },
            inGame);

        vr::VREvent_t ev;
        while (sys->PollNextEvent(&ev, sizeof ev)) {
            if (ev.eventType == vr::VREvent_Quit) {
                sys->AcknowledgeQuit_Exiting();

                overlays.Stop();
                vr::VR_Shutdown();
                return 0;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

int main() {
    Pointer p;
    p.Init();
    return p.Run();
}
