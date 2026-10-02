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
#include "pointer.h"

namespace {

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

}  // namespace

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

// The headset off: SteamVR drops the HMD's activity to idle as soon as it comes off.
// An awake pointer (a connected controller, SteamVR's laser mode forced on) kept the
// displays from sleeping, so it's released at once, and the mouse can't wake it until
// the headset is back on (then the first mouse input does).
void Pointer::HeadsetOff() {
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
}

// This frame's poses and time (see Frame).
void Pointer::ReadFrame(Frame &frame) {
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0.011f, frame.all, vr::k_unMaxTrackedDeviceCount);
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseRawAndUncalibrated, 0.011f, &frame.hmdRaw, 1);
    frame.tnow = Clock::now();
    const auto &hm = frame.Hmd().mDeviceToAbsoluteTracking.m;
    frame.eye = {hm[0][3], hm[1][3], hm[2][3]};
}

// Claim pulse (switchlaserhand on the driver's "a" button, no click).
void Pointer::ClaimPulse(const Frame &frame) {
    const auto tnow = frame.tnow;
    if (claimPending && tnow >= claimAt) {
        SendTo(out, "ft_pointer", "btn a 1");
        claimPending = false;
        claimHeld = true;
        claimRelease = tnow + std::chrono::milliseconds(60);
    } else if (claimHeld && tnow >= claimRelease) {
        SendTo(out, "ft_pointer", "btn a 0");
        claimHeld = false;
    }
}

// The overlay list is only needed while the pointer is awake (see OverlayList).
void Pointer::PauseOverlayList() {
    overlays.SetPaused(!active || headsetOff);
}

// Laser mode on while the pointer is awake.
void Pointer::LaserMode() {
    if (active != laserModeShown) {
        laserModeShown = active;
        if (active) overlay->ShowOverlay(laserMode);
        else overlay->HideOverlay(laserMode);
        if (debug) std::printf("laser mode %s\n", active ? "forced on" : "released");
        if (debug) std::fflush(stdout);
    }
}

// Didn't get the hand role (a held controller keeps it): release, back off.
void Pointer::HandRole(const Frame &frame) {
    const auto tnow = frame.tnow;
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
}

// Last used wins: a real controller being moved releases the pointer (see the top),
// in gaze mode too.
void Pointer::LastUsedWins(const Frame &frame) {
    const auto &all = frame.all;
    const auto tnow = frame.tnow;
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
}

// A press holds the pointer (gaze mode's dot shows meanwhile, see the top).
void Pointer::MarkHeld(const Frame &frame) {
    const auto tnow = frame.tnow;
    if (aimHeld || leftHeld || clickPress || clickRelease) lastHeld = tnow;
}

// Frame controller buttons (vrbuttons.h), to the relay.
void Pointer::PollControllerButtons() {
    controllerButtons.Poll(
        [&](const char *button, bool down) {
            SendTo(out, "frametop_relay", std::string("vrbtn ") + button + (down ? " 1" : " 0"));
        },
        inGame);
}

// SteamVR quits: shut down (true: main returns).
bool Pointer::SteamVRQuit() {
    vr::VREvent_t ev;
    while (sys->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) {
            sys->AcknowledgeQuit_Exiting();

            overlays.Stop();
            vr::VR_Shutdown();
            return true;
        }
    }
    return false;
}

int main() {
    Pointer p;
    p.Init();
    // Each frame: the sections in this order (Pointer's members above).
    while (true) {
        p.HeadsetOff();
        p.GazeAwake();
        p.RelayCommands();
        Frame frame;
        p.ReadFrame(frame);
        p.ClaimPulse(frame);
        p.PauseOverlayList();
        p.LaserMode();
        p.HandRole(frame);
        p.LastUsedWins(frame);
        p.Recenter(frame);
        p.GazeMode(frame);
        p.Hands(frame);
        p.GazePrecisionButtons(frame);
        p.KeyboardClicks(frame);
        p.CalPanelOpened();
        p.HeadSteer(frame);
        p.PinchesAndGrips(frame);
        p.HandsStopped(frame);
        p.MarkHeld(frame);
        p.HeadFollow(frame);
        p.SlowWork();
        p.Cursor(frame);
        p.HeldBackPress(frame);
        p.KeyboardClickHold(frame);
        p.Click(frame);
        p.PollControllerButtons();
        if (p.SteamVRQuit()) return 0;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}
