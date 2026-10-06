// The OpenVR side of ft-screens: one overlay per screen, client DMA-BUFs imported with
// IVRIPCResourceManagerClient::ImportDmabuf (no copy, no size limit), panel mouse events
// turned into ft_events for the compositor, and the panels' own handling:
//   - a grab bar under each screen: press it with any laser (a controller, or the 3D
//     mouse's virtual controller) and the screen follows that device rigidly until the
//     release, so the 3D mouse's tilt (right button while dragging) turns it; scrolling
//     while dragging pushes it away or pulls it closer (along the line from the head).
//   - a curve button next to the bar: bends the screen into a cylinder around you (its
//     radius: your distance to it when pressed), or flat again.
//   - a roll button next to that: drag it sideways like a knob to roll the screen about
//     its centre (it snaps level within kRollSnap), or scroll on it for kRollStep steps.
//   - a resize tab on the bottom right corner: drag it to set the width (the height
//     follows the screen's resolution).
//   - a reset button left of the bar: every screen back in its layout, around where you
//     are now (`ft-layout apply`, like Meta+Shift+R).
//   The controls are translucent, like SteamVR's own, and brighten under a laser. They
//   are invisible until a laser (a controller's, or the 3D mouse's) lands on or passes very close to
//   one of them (UpdateControls).
//   - pin to a wrist: while carrying a screen, sweep the laser (the line from the carrying
//     device to the bar) across your other controller. A ring around each controller
//     shows the target and a dot where the laser passes it; crossing the ring arms the pin
//     (ring and bar turn blue), crossing it again disarms it. Let go while armed and the
//     screen rides on that controller as it is then, at any size and distance, so you can
//     arm it and then turn it the way you want before letting go. Grabbing a pinned
//     screen keeps it armed for its wrist: move it, let go, and it's re-pinned there
//     (sweep across the ring to take it off). A pinned screen shows only while you see
//     its front, within the wrist angle (and fades out over the last kFade degrees).
//   - pin to your head (the pin command, from ft-layout and Frametop Display Settings): the
//     screen rides on the headset as it is then, like a HUD, and shows whenever the
//     screens do. Carrying it works like a wrist pin: let go and it's re-pinned to your
//     head where you put it; sweep across a wrist ring to move it to that wrist, or twice
//     to leave it in the room.
//   - visibility modes: always (the hide hotkey toggles), only with the SteamVR dashboard
//     open, while you look at a chosen controller (the wrist gesture), or toggle only
//     (hidden until the hotkey shows them).
//   - a screen hidden on its own ("conceal <screen>", from ft-layout and profiles) stays
//     hidden whatever the mode or the hotkey says, until "reveal <screen>". (Not "hide
//     <screen>": an older build reads anything starting with "hide" as the hotkey's hide.)
//   - controllers on the screens: while visible, the screens can keep SteamVR's laser mouse
//     on (VROverlayFlags_MakeOverlaysInteractiveIfVisible), so controllers use them with
//     the dashboard closed. That also takes the controllers away from a VR game, so by
//     default it's off while a game (a scene app) runs: the screens stay up over the game,
//     the controllers stay in it, and the 3D mouse (its own laser mode) or the dashboard
//     works the screens. Modes: always, outside_games (default), dashboard (never on its
//     own; also for flatscreen games, which aren't scene apps). Where the mode leaves the
//     controllers to the game, pointing a controller at a panel (a screen, a floating window
//     and its popups, their controls, the keyboard) turns the laser on for it until you
//     point away, like SteamVR's own floating windows (UpdateAim).
//   - during a VR game the screens hide unless the dashboard is open (g_inGames, default),
//     or stay visible over it; the hotkey still shows them.
//   - paused ("pause on", from the input relay when Frametop pauses for a VR game,
//     input/game_pause.py): every screen and floating window hides whatever the mode, the
//     hotkey, or the dashboard says, and compositor.c gives KWin a frame callback once a
//     second, as for any hidden screen, so KWin and its apps hardly draw. "pause off" undoes
//     it.
//   - hand cutouts (handcut.cpp): where ft-hands (hands/) tracks a hand between an eye and a
//     screen, that eye sees through the screen (to Room View). Only then is the screen
//     drawn by us, into a side-by-side buffer (one half per eye); otherwise its client
//     buffer is shown as is.
//   - the catcher: a button pressed on a screen is released in KWin even when the laser
//     lets go between panels (UpdateCatcher).
//   - floating windows (docs/floating-windows.md): KWin's spare outputs, after the screens,
//     are panels too, for one window each. ft-floatd sizes the output to the window plus a
//     margin and tells us the window's rectangle ("float"): the panel shows only that crop of
//     the buffer (SetOverlayTextureBounds), at the density of the screen it came from, and
//     each popup or dialog gets a small panel of its own over it, cut from the same buffer
//     ("sub"). Pressing its title bar carries the panel like the bar does, while KWin's
//     pointer stays put, so the window doesn't move on its output. The corner tab resizes the
//     window (in pixels, at the same density) instead of scaling the panel, and two more
//     buttons close it and put it back on the desktop (both through ft-floatd).
// OpenVR has no overlay-relative transforms here (openvr v2.15.6), so the bar, button,
// and handle are placed whenever their screen moves.
#include "vr.h"

#include "handcut.h"
#include "keyboard.h"

#include <drm_fourcc.h>
#include <openvr.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <spawn.h>

extern char **environ;  // for posix_spawn

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

using Mat = vr::HmdMatrix34_t;
using Clock = std::chrono::steady_clock;

Mat Identity() {
    Mat m{};
    m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
    return m;
}
Mat Mul(const Mat &a, const Mat &b) {
    Mat r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            double v = j == 3 ? a.m[i][3] : 0;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = float(v);
        }
    }
    return r;
}
Mat Inverse(const Mat &a) {  // rigid: R^T, -R^T t
    Mat r{};
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; ++i) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}
Mat Translation(double x, double y, double z) {
    Mat m = Identity();
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}
double Dot3(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Column(const Mat &m, int c, double out[3]) { out[0] = m.m[0][c], out[1] = m.m[1][c], out[2] = m.m[2][c]; }

// A panel pose from a centre and the direction its front is seen from (yaw, pitch; see
// layout: the front faces back along that direction), turned by roll.
Mat PanelPose(double x, double y, double z, double yawDeg, double pitchDeg, double rollDeg) {
    const double yw = yawDeg * M_PI / 180, pt = pitchDeg * M_PI / 180, rl = rollDeg * M_PI / 180;
    const double fx = -std::sin(yw) * std::cos(pt), fy = std::sin(pt), fz = -std::cos(yw) * std::cos(pt);
    const double Z[3] = {-fx, -fy, -fz};  // the front
    double X[3] = {Z[2], 0, -Z[0]};       // up x Z: horizontal right
    const double n = std::sqrt(X[0] * X[0] + X[2] * X[2]) + 1e-12;
    X[0] /= n, X[2] /= n;
    const double Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
    const double c = std::cos(rl), s = std::sin(rl);
    Mat m{};
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = float(X[i] * c + Y[i] * s);
        m.m[i][1] = float(Y[i] * c - X[i] * s);
        m.m[i][2] = float(Z[i]);
    }
    m.m[0][3] = float(x), m.m[1][3] = float(y), m.m[2][3] = float(z);
    return m;
}

// Device poses, read once per tick (ft_vr_poll) or per command.
vr::TrackedDevicePose_t g_poses[vr::k_unMaxTrackedDeviceCount];
void RefreshPoses() {
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, g_poses,
                                                    vr::k_unMaxTrackedDeviceCount);
}
bool DevicePose(vr::TrackedDeviceIndex_t dev, Mat *out) {
    if (dev >= vr::k_unMaxTrackedDeviceCount || !g_poses[dev].bPoseIsValid) return false;
    *out = g_poses[dev].mDeviceToAbsoluteTracking;
    return true;
}
// Where a device's laser starts and points: SteamVR's laser comes from its render model's
// "tip" component, not the device pose. On the Frame's controllers the tip points 40 degrees
// below the pose's -Z, so rays from the pose missed what the laser was on. Devices without
// a tip (the 3D mouse's virtual controller) aim along their pose. Cached per device; a
// model that isn't loaded yet is asked again a few seconds later.
struct Tip {
    std::string model;
    Mat offset = Identity();
    bool found = false;
    Clock::time_point checked;
};
Mat TipOffset(vr::TrackedDeviceIndex_t dev) {
    static std::map<vr::TrackedDeviceIndex_t, Tip> cache;
    char model[256] = "";
    vr::VRSystem()->GetStringTrackedDeviceProperty(dev, vr::Prop_RenderModelName_String, model, sizeof model);
    const auto now = Clock::now();
    auto it = cache.find(dev);
    if (it != cache.end() && it->second.model == model &&
        (it->second.found || now - it->second.checked < std::chrono::seconds(5)))
        return it->second.offset;
    Tip tip{model, Identity(), false, now};
    vr::RenderModel_ControllerMode_State_t mode{};
    vr::RenderModel_ComponentState_t state{};
    // GetComponentState, not GetComponentStateForDevicePath: without an input source handle
    // the latter fails for every component while a VR game runs, and the rays came from the
    // pose, 40 degrees above the laser. The tip doesn't move with the buttons.
    vr::VRControllerState_t buttons{};
    if (model[0] && vr::VRRenderModels()->GetComponentState(model, vr::k_pch_Controller_Component_Tip, &buttons, &mode,
                                                            &state))
        tip.offset = state.mTrackingToComponentLocal, tip.found = true;
    cache[dev] = tip;
    return tip.offset;
}
bool LaserPose(vr::TrackedDeviceIndex_t dev, Mat *out) {
    Mat d;
    if (!DevicePose(dev, &d)) return false;
    *out = Mul(d, TipOffset(dev));
    return true;
}

bool IsHandController(vr::TrackedDeviceIndex_t i) {
    if (vr::VRSystem()->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) return false;
    char type[64] = "";
    vr::VRSystem()->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
    return std::strcmp(type, "ft_pointer") != 0;  // not the 3D mouse's virtual controller
}
// "left", "right", or "head" (the headset) -> the device to pin to.
vr::TrackedDeviceIndex_t HandDevice(const char *hand) {
    if (std::strcmp(hand, "head") == 0) return vr::k_unTrackedDeviceIndex_Hmd;
    return vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(
        std::strcmp(hand, "right") == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
}
const char *HandName(vr::TrackedDeviceIndex_t i) {
    if (i == vr::k_unTrackedDeviceIndex_Hmd) return "head";
    switch (vr::VRSystem()->GetControllerRoleForTrackedDeviceIndex(i)) {
        case vr::TrackedControllerRole_LeftHand: return "left";
        case vr::TrackedControllerRole_RightHand: return "right";
        default: return "none";
    }
}

enum class Drag { None, Move, Resize, Roll };
enum class Mode { Always, Dashboard, Gesture, Toggle };
enum class Lasers { Always, OutsideGames, Dashboard };
enum class InGames { Visible, Hide };

constexpr double kWristZone = 0.06;  // the laser passing this close to a controller is on its wrist
constexpr double kWristLeave = 0.09; // ...and has left it beyond this (so it doesn't flicker)
constexpr double kDotRange = 0.35;   // the guide dot shows while the laser is this close
constexpr double kMinWidth = 0.15;
constexpr double kFade = 10;         // degrees over which a pinned screen fades out
constexpr double kRollSnap = 2.5;    // degrees from level where rolling snaps level
constexpr double kRollStep = 5;      // degrees per scroll notch on the roll button
constexpr float kChromeIdle = 0.55f; // the controls' opacity without a laser on them
constexpr long kControlsLinger = 35; // ticks (~0.4 s) the controls stay after a laser leaves
constexpr long kAimLinger = 25;      // ticks (~0.3 s) a panel keeps the laser on after the aim leaves it
long g_tick = 0;                     // ft_vr_poll calls
bool g_vr = false;                   // connected to SteamVR (ft-screens --no-vr runs without it)
constexpr vr::TrackedDeviceIndex_t kNone = vr::k_unTrackedDeviceIndexInvalid;

// A popup or dialog of a floating window: a small panel over it, cut from the same buffer.
struct Sub {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int x = 0, y = 0, w = 0, h = 0;  // in the output's buffer, pixels
};

struct Screen {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid, bar = vr::k_ulOverlayHandleInvalid,
                          handle = vr::k_ulOverlayHandleInvalid, curveButton = vr::k_ulOverlayHandleInvalid,
                          rollButton = vr::k_ulOverlayHandleInvalid, dockButton = vr::k_ulOverlayHandleInvalid,
                          closeButton = vr::k_ulOverlayHandleInvalid,  // the last two: floating windows
                          resetButton = vr::k_ulOverlayHandleInvalid;  // desktop screens only
    int width = 0, height = 0;    // current buffer size (mouse scale)
    double metres = 1;
    double curve = 0;             // cylinder radius in metres; 0 = flat
    const void *shown = nullptr;  // a frame arrived
    bool visible = false;         // shown in VR right now
    bool alone = false;           // hidden on its own (conceal <screen>), whatever the mode
    float alpha = 1;
    vr::TrackedDeviceIndex_t pinned = kNone;  // riding on this controller
    Mat pinRel = Identity();                  // controller -> screen
    Mat pose = Identity();                    // where it is in the room, when not pinned
    Drag drag = Drag::None;
    vr::TrackedDeviceIndex_t dragDevice = kNone;
    Mat dragRel = Identity();                 // device -> screen, while moving
    double grabX = 0, grabY = 0;              // resize: the grab point relative to the corner
    Mat rollFrom = Identity();                // roll: the pose at the press (pinRel when pinned)
    double rollAngle = 0;                     // roll: the laser's angle around the centre then
    bool hover[7] = {};                       // a laser is on the bar, curve, roll, resize, dock, close, reset control
    bool lasers = true;                       // MakeOverlaysInteractiveIfVisible is set
    float controls = 0;                       // the controls' fade, 0 (hidden) .. 1
    bool controlsUp = false;                  // the controls' overlays are shown
    long nearUntil = 0;                       // a laser was near the controls until this tick
    long aimUntil = 0;                        // a hand controller pointed at it until this tick (UpdateAim)
    vr::TrackedDeviceIndex_t pinTarget = kNone;  // moving: rides on this controller when let go
    vr::TrackedDeviceIndex_t onWrist = kNone;    // moving: the laser is in this controller's ring
    bool barLit = false;
    const void *key = nullptr;    // the client buffer on it now, and its dmabuf (for cutouts)
    ft_dmabuf buf{};
    vr::SharedTextureHandle_t plain = 0;  // that buffer's SteamVR import
    bool cutting = false;         // showing a cutout buffer (side by side) instead
    double chrome = 0.3;          // the bar's width; the other controls follow it (ChromeSize)
    double grip = 0.04;           // the corner tab's and the round buttons' size
    // A floating window's panel (see the top): the window's rectangle in the buffer, its
    // title bar's height there, and the density.
    bool floating = false;        // a spare output's panel
    bool floatOn = false;         // ft-floatd has a window on it ("float" .. "unfloat")
    bool outputOn = false;        // KWin has the spare output turned on
    bool minimized = false;
    int cropX = 0, cropY = 0, cropW = 0, cropH = 0;
    int titleH = 0;
    double mpp = 0;               // metres per buffer pixel
    bool titleCarry = false;      // carried by its title bar: KWin's pointer stays at carryX, carryY
    double carryX = 0, carryY = 0;
    long resizeSent = 0;          // g_tick of the last resize request (they're throttled)
    int resizeW = 0, resizeH = 0; // ...and its size
    std::map<int, Sub> subs;
    // Attention (UpdateAttention): what ft_vr_screen_attention answers, and until when (ms)
    // it stays focused or in view after the last reason for it.
    ft_attention attention = FT_FOCUSED;
    int64_t inputMs = INT64_MIN / 2;  // the last pointer event on it or its controls
    int64_t focusUntil = 0, viewUntil = 0;
    double heightMetres() const {
        if (floating && cropW > 0) return metres * cropH / cropW;
        return width > 0 ? metres * height / width : metres * 9 / 16;
    }
    // Buffer pixels from OpenVR's mouse position on the panel (its origin is bottom left).
    // A cropped panel too: SteamVR gives the position in the whole texture, not the crop.
    void ToBuffer(double mx, double my, double *x, double *y) const { *x = mx, *y = height - my; }
    std::array<vr::VROverlayHandle_t, 7> Controls() const {
        return {bar, curveButton, rollButton, handle, dockButton, closeButton, resetButton};
    }
    std::array<vr::VROverlayHandle_t, 8> All() const {
        return {overlay, bar, curveButton, rollButton, handle, dockButton, closeButton, resetButton};
    }
};
std::map<int, Screen> g_screens;
std::map<const void *, vr::SharedTextureHandle_t> g_imports;

// Hand cutouts (see the top and handcut.h).
bool g_cutouts = true;          // the cutouts command turns them off
handcut::Hands g_hands;
handcut::Renderer g_cutter;
int g_cutterState = 0;          // 0 not tried, 1 ready, -1 unavailable
std::map<const void *, vr::SharedTextureHandle_t> g_cutImports;

// Visibility (see the top). g_manual is the hide/show switch: in the always mode it hides
// the screens, in the others it shows them anyway.
Mode g_mode = Mode::Always;
bool g_manual = false;
double g_wristAngle = 60;    // a pinned screen shows while you see its front within this
double g_gestureAngle = 20;  // gesture: look within this of the controller
std::string g_gestureHand = "left";
Lasers g_lasers = Lasers::OutsideGames;  // when controllers' lasers work the screens (see the top)
bool g_gameRunning = false;              // a scene app (VR game) is running
bool g_paused = false;                   // Frametop paused for a VR game: everything hidden (see the top)
InGames g_inGames = InGames::Hide;       // during a VR game, the always mode acts like the dashboard mode

// ---------------------------------------------------------------- chrome (bar, button, handle)

// The controls look like SteamVR's own: a light translucent pill for the bar, dark
// translucent discs with white glyphs for the buttons (the overlay alpha, kChromeIdle,
// dims them further until a laser is on them).
std::vector<uint8_t> PillTexture(int w, int h, uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha) {
    std::vector<uint8_t> px(size_t(w) * h * 4, 0);
    const double r = h / 2.0 - 1;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const double cx = std::clamp(double(x), r + 1, w - r - 1), cy = h / 2.0;
            const double d = std::hypot(x + 0.5 - cx, y + 0.5 - cy);
            uint8_t *p = &px[(size_t(y) * w + x) * 4];
            p[0] = red, p[1] = green, p[2] = blue;
            p[3] = uint8_t(std::clamp(r - d + 0.5, 0.0, 1.0) * alpha);
        }
    return px;
}
const std::vector<uint8_t> &BarTexture(bool lit) {
    static const auto normal = PillTexture(256, 24, 235, 235, 235, 210), glow = PillTexture(256, 24, 90, 170, 255, 240);
    return lit ? glow : normal;
}

// Paint a control: dark translucent inside `inside(u, v)`, white where `glyph(u, v)`, a
// faint light rim where `rim(u, v)`. u, v: -1..1 across the texture, v up.
template <typename In, typename Glyph, typename Rim>
std::vector<uint8_t> ControlTexture(int n, In inside, Glyph glyph, Rim rim) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    const int ss = 3;  // supersampling, for smooth edges
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            double in = 0, g = 0, e = 0;
            for (int j = 0; j < ss; ++j)
                for (int i = 0; i < ss; ++i) {
                    const double u = (x + (i + 0.5) / ss) / n * 2 - 1, v = 1 - (y + (j + 0.5) / ss) / n * 2;
                    if (!inside(u, v)) continue;
                    in += 1;
                    if (glyph(u, v)) g += 1;
                    else if (rim(u, v)) e += 1;
                }
            const double k = ss * ss;
            in /= k, g /= k, e /= k;
            uint8_t *p = &px[(size_t(y) * n + x) * 4];
            const double bg = in - g - e;  // dark part
            const double a = bg * 0.72 + e * 0.6 + g * 1.0;
            if (a <= 0) continue;
            const double shade = (bg * 0.72 * 38 + e * 0.6 * 200 + g * 255) / a;
            p[0] = p[1] = p[2] = uint8_t(std::clamp(shade, 0.0, 255.0));
            p[3] = uint8_t(std::clamp(a * 255, 0.0, 255.0));
        }
    return px;
}
bool InDisc(double u, double v) { return u * u + v * v <= 1; }
bool DiscRim(double u, double v) { return u * u + v * v > 0.86 * 0.86; }

std::vector<uint8_t> CornerTexture(int n) {
    // A quarter disc whose corner (the texture's top left) sits on the screen's bottom
    // right corner, with two grip arcs: "drag this corner".
    auto r = [](double u, double v) { return std::hypot(u + 1, v - 1) / 2; };  // 0..1 from the corner
    return ControlTexture(
        n, [&](double u, double v) { return r(u, v) <= 1; },
        [&](double u, double v) {
            const double d = r(u, v);
            return std::fabs(d - 0.5) < 0.035 || std::fabs(d - 0.75) < 0.035;
        },
        [&](double u, double v) { return r(u, v) > 0.93; });
}

std::vector<uint8_t> CurveTexture(int n) {
    // An arc: "curve this screen".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) { return std::fabs(std::hypot(u, -v - 1.9) - 1.7) < 0.11 && std::fabs(u) < 0.6; },
        DiscRim);
}

std::vector<uint8_t> RollTexture(int n) {
    // A circular arrow, counterclockwise: "roll this screen".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            const double r = std::hypot(u, v);
            double ang = std::atan2(v, u) * 180 / M_PI;
            if (ang < 0) ang += 360;
            if (std::fabs(r - 0.48) < 0.085 && ang >= 100) return true;  // the arc, 100..360 degrees
            // The head at 0 degrees, pointing up (the way the arc turns there).
            const double hx = u - 0.48, hy = v + 0.02;
            return hy >= 0 && hy <= 0.3 && std::fabs(hx) <= 0.24 * (1 - hy / 0.3);
        },
        DiscRim);
}

std::vector<uint8_t> CloseTexture(int n) {
    // A cross: "close this window".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            return std::max(std::fabs(u), std::fabs(v)) < 0.42 &&
                   (std::fabs(u - v) < 0.12 || std::fabs(u + v) < 0.12);
        },
        DiscRim);
}

std::vector<uint8_t> DockTexture(int n) {
    // An arrow down onto a line: "back to the desktop".
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            if (std::fabs(u) < 0.5 && v > -0.52 && v < -0.38) return true;  // the line
            if (std::fabs(u) < 0.08 && v > -0.1 && v < 0.5) return true;    // the shaft
            return v >= -0.3 && v <= -0.05 && std::fabs(u) <= (v + 0.3) * 1.2;  // the head, point down
        },
        DiscRim);
}

std::vector<uint8_t> ResetTexture(int n) {
    // A reticle: "put the screens back around you" (like a recenter).
    return ControlTexture(
        n, InDisc,
        [](double u, double v) {
            const double r = std::hypot(u, v);
            if (std::fabs(r - 0.4) < 0.07 || r < 0.13) return true;  // the ring and the centre
            return (std::fabs(u) < 0.06 && std::fabs(v) > 0.47 && std::fabs(v) < 0.72) ||
                   (std::fabs(v) < 0.06 && std::fabs(u) > 0.47 && std::fabs(u) < 0.72);  // the ticks
        },
        DiscRim);
}

vr::VROverlayHandle_t MakeChrome(const char *key, const char *name, const std::vector<uint8_t> &px, int w, int h) {
    vr::VROverlayHandle_t o = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->CreateOverlay(key, name, &o) != vr::VROverlayError_None) return o;
    vr::VROverlay()->SetOverlayRaw(o, const_cast<uint8_t *>(px.data()), uint32_t(w), uint32_t(h), 4);
    vr::VROverlay()->SetOverlayInputMethod(o, vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlaySortOrder(o, 10);
    return o;
}

void LightBar(Screen &s, bool lit) {
    if (s.barLit == lit) return;
    s.barLit = lit;
    const auto &px = BarTexture(lit);
    vr::VROverlay()->SetOverlayRaw(s.bar, const_cast<uint8_t *>(px.data()), 256, 24, 4);
}

// ---------------------------------------------------------------- wrist guides

// While a screen is carried, each other controller gets a ring (its wrist zone, facing
// you) and a dot where the laser passes closest to it. Blue: armed / in the ring.
std::vector<uint8_t> DiscTexture(int n, double stroke, uint8_t red, uint8_t green, uint8_t blue, uint8_t fill,
                                 uint8_t rimShade) {
    std::vector<uint8_t> px(size_t(n) * n * 4, 0);
    const double c = n / 2.0, r = n / 2.0 - 1;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const double d = std::hypot(x + 0.5 - c, y + 0.5 - c);
            const double a = std::clamp(r - d + 0.5, 0.0, 1.0);
            uint8_t *p = &px[(size_t(y) * n + x) * 4];
            const bool rim = d > r - stroke;
            const bool edge = d > r - 2 || (rim && d < r - stroke + 2);  // a dark line each side of the rim
            p[0] = edge ? rimShade : red, p[1] = edge ? rimShade : green, p[2] = edge ? rimShade : blue;
            p[3] = uint8_t(a * (rim ? 235 : fill));
        }
    return px;
}
const std::vector<uint8_t> &RingTexture(bool lit) {
    static const auto normal = DiscTexture(128, 9, 240, 240, 240, 40, 60),
                      glow = DiscTexture(128, 12, 90, 170, 255, 110, 30);
    return lit ? glow : normal;
}
const std::vector<uint8_t> &DotTexture(bool lit) {
    static const auto normal = DiscTexture(32, 16, 250, 250, 250, 250, 50),
                      glow = DiscTexture(32, 16, 90, 170, 255, 250, 30);
    return lit ? glow : normal;
}

struct GuidePart {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid;
    int lit = -1;  // the texture on it (-1: none yet)
    bool shown = false;
    void Show(bool on) {
        if (on == shown || overlay == vr::k_ulOverlayHandleInvalid) return;
        shown = on;
        if (on) vr::VROverlay()->ShowOverlay(overlay);
        else vr::VROverlay()->HideOverlay(overlay);
    }
    void Light(bool on, const std::vector<uint8_t> &px, int n) {
        if (int(on) == lit) return;
        lit = on;
        vr::VROverlay()->SetOverlayRaw(overlay, const_cast<uint8_t *>(px.data()), uint32_t(n), uint32_t(n), 4);
    }
};
struct Guide { GuidePart ring, dot; };
std::map<vr::TrackedDeviceIndex_t, Guide> g_guides;

Guide &GuideFor(vr::TrackedDeviceIndex_t dev) {
    auto it = g_guides.find(dev);
    if (it != g_guides.end()) return it->second;
    Guide &g = g_guides[dev];
    char key[64];
    std::snprintf(key, sizeof key, "frametop.guide.%u.ring", dev);
    if (vr::VROverlay()->CreateOverlay(key, "Wrist pin target", &g.ring.overlay) == vr::VROverlayError_None) {
        vr::VROverlay()->SetOverlayWidthInMeters(g.ring.overlay, float(2 * kWristZone));
        vr::VROverlay()->SetOverlaySortOrder(g.ring.overlay, 20);
    }
    std::snprintf(key, sizeof key, "frametop.guide.%u.dot", dev);
    if (vr::VROverlay()->CreateOverlay(key, "Wrist pin laser", &g.dot.overlay) == vr::VROverlayError_None) {
        vr::VROverlay()->SetOverlayWidthInMeters(g.dot.overlay, 0.022f);
        vr::VROverlay()->SetOverlaySortOrder(g.dot.overlay, 21);
    }
    return g;
}

// A pose at pt facing the head (upright).
Mat FacingPose(const double pt[3], const Mat &head) {
    double z[3] = {head.m[0][3] - pt[0], head.m[1][3] - pt[1], head.m[2][3] - pt[2]};
    const double zl = std::sqrt(Dot3(z, z)) + 1e-9;
    for (double &v : z) v /= zl;
    double x[3] = {z[2], 0, -z[0]};  // up x z
    const double xl = std::sqrt(x[0] * x[0] + x[2] * x[2]);
    if (xl < 1e-6) x[0] = 1, x[2] = 0;
    else x[0] /= xl, x[2] /= xl;
    const double y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
    Mat m{};
    for (int i = 0; i < 3; ++i) m.m[i][0] = float(x[i]), m.m[i][1] = float(y[i]), m.m[i][2] = float(z[i]), m.m[i][3] = float(pt[i]);
    return m;
}

void ApplyCurve(const Screen &s) {
    // OpenVR's curvature: the fraction of a full cylinder the overlay's width covers.
    const double c = s.curve > 0 ? std::clamp(s.metres / (2 * M_PI * s.curve), 0.0, 1.0) : 0.0;
    vr::VROverlay()->SetOverlayCurvature(s.overlay, float(c));
}

// The screen's pose in the room (a pinned one: its controller's pose times pinRel).
bool ScreenPose(const Screen &s, Mat *out) {
    if (s.pinned != kNone) {
        Mat d;
        if (!DevicePose(s.pinned, &d)) return false;
        *out = Mul(d, s.pinRel);
        return true;
    }
    // Our own copy: reading it back from SteamVR right after setting it could return the
    // old pose, which left a moved screen's controls behind.
    *out = s.pose;
    return true;
}

// The controls' size from both the screen's width and its distance from the head (the
// geometric mean of 12% of the width and 10% of the distance), so a small screen near you
// gets small controls and a big or far one gets big ones, never under about 1.7 degrees.
void ChromeSize(Screen &s) {
    Mat head, p;
    double dist = 2;
    if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) && ScreenPose(s, &p)) {
        const double d[3] = {p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3]};
        dist = std::max(0.2, std::sqrt(Dot3(d, d)));
    }
    const double least = dist * 0.03;
    s.chrome = std::clamp(std::sqrt(0.012 * dist * s.metres), least, std::max(least, s.metres * 0.5));
    s.grip = std::max(s.chrome * 0.13, dist * 0.018);
}

// A point on the screen's surface, u metres along it from the centre (along the arc when
// curved), v up, dz out of it, facing the way the surface does there. OpenVR curves a
// screen into a cylinder toward its front, with its centre line where the flat one was.
Mat OnSurface(const Screen &s, double u, double v, double dz) {
    if (s.curve <= 0) return Translation(u, v, dz);
    const double r = s.curve, a = u / r, c = std::cos(a), sn = std::sin(a);
    Mat m = Identity();
    m.m[0][0] = float(c), m.m[0][2] = float(-sn);
    m.m[2][0] = float(sn), m.m[2][2] = float(c);
    m.m[0][3] = float(r * sn - dz * sn), m.m[1][3] = float(v), m.m[2][3] = float(r - r * c + dz * c);
    return m;
}

double BarY(const Screen &s) { return -(s.heightMetres() / 2 + s.chrome * 0.06 + s.chrome * 12 / 256); }
Mat BarOffset(const Screen &s) { return OnSurface(s, 0, BarY(s), 0.003); }

// Put the bar, the curve button, and the corner tab under the screen (same parent: the
// room or the controller), sized for the screen and its distance, and on its surface.
// Where each control sits, relative to the screen: bar, curve, roll, resize tab, a
// floating window's dock and close buttons (left of the bar), and a desktop screen's reset
// button (left of the bar, where a floating window has its dock button).
std::array<Mat, 7> ControlOffsets(const Screen &s) {
    const double h = s.heightMetres(), bar = s.chrome, button = s.grip, gap = bar * 0.06;
    return {BarOffset(s), OnSurface(s, bar / 2 + gap + button / 2, BarY(s), 0.003),
            OnSurface(s, bar / 2 + gap * 2 + button * 1.5, BarY(s), 0.003),
            // The tab's top left corner is the screen's bottom right corner.
            OnSurface(s, s.metres / 2 + s.grip / 2, -(h / 2 + s.grip / 2), 0.003),
            OnSurface(s, -(bar / 2 + gap + button / 2), BarY(s), 0.003),
            OnSurface(s, -(bar / 2 + gap * 2 + button * 1.5), BarY(s), 0.003),
            OnSurface(s, -(bar / 2 + gap + button / 2), BarY(s), 0.003)};
}

// A floating window's popups and dialogs, a few millimetres in front of it, where they are
// in the buffer relative to the window.
void PlaceSubs(const Screen &s) {
    if (s.subs.empty() || s.cropW <= 0) return;
    Mat p;
    if (s.pinned == kNone && !ScreenPose(s, &p)) return;
    for (const auto &[k, sub] : s.subs) {
        const double u = (sub.x + sub.w / 2.0 - (s.cropX + s.cropW / 2.0)) * s.mpp;
        const double v = -(sub.y + sub.h / 2.0 - (s.cropY + s.cropH / 2.0)) * s.mpp;
        const Mat off = OnSurface(s, u, v, 0.005);
        vr::VROverlay()->SetOverlayWidthInMeters(sub.overlay, float(std::max(0.01, sub.w * s.mpp)));
        if (s.pinned != kNone) {
            const Mat m = Mul(s.pinRel, off);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(sub.overlay, s.pinned, &m);
        } else {
            const Mat m = Mul(p, off);
            vr::VROverlay()->SetOverlayTransformAbsolute(sub.overlay, vr::TrackingUniverseStanding, &m);
        }
    }
}

void PlaceChrome(Screen &s) {
    ChromeSize(s);
    const double bar = s.chrome, button = s.grip;
    const auto offsets = ControlOffsets(s);
    vr::VROverlay()->SetOverlayWidthInMeters(s.bar, float(bar));
    vr::VROverlay()->SetOverlayWidthInMeters(s.curveButton, float(button));
    vr::VROverlay()->SetOverlayWidthInMeters(s.rollButton, float(button));
    vr::VROverlay()->SetOverlayWidthInMeters(s.handle, float(s.grip));
    if (s.floating) {
        vr::VROverlay()->SetOverlayWidthInMeters(s.dockButton, float(button));
        vr::VROverlay()->SetOverlayWidthInMeters(s.closeButton, float(button));
    } else {
        vr::VROverlay()->SetOverlayWidthInMeters(s.resetButton, float(button));
    }
    // Curved, the bar bends with the screen's bottom edge.
    vr::VROverlay()->SetOverlayCurvature(s.bar, s.curve > 0 ? float(std::min(1.0, bar / (2 * M_PI * s.curve))) : 0.f);
    const std::pair<vr::VROverlayHandle_t, Mat> parts[] = {
        {s.bar, offsets[0]},        {s.curveButton, offsets[1]}, {s.rollButton, offsets[2]},
        {s.handle, offsets[3]},     {s.dockButton, offsets[4]},  {s.closeButton, offsets[5]},
        {s.resetButton, offsets[6]}};
    PlaceSubs(s);
    if (s.pinned != kNone) {
        for (const auto &[o, off] : parts) {
            if (o == vr::k_ulOverlayHandleInvalid) continue;
            const Mat m = Mul(s.pinRel, off);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(o, s.pinned, &m);
        }
        return;
    }
    Mat p;
    if (!ScreenPose(s, &p)) return;
    for (const auto &[o, off] : parts) {
        if (o == vr::k_ulOverlayHandleInvalid) continue;
        const Mat m = Mul(p, off);
        vr::VROverlay()->SetOverlayTransformAbsolute(o, vr::TrackingUniverseStanding, &m);
    }
}

// Screens you walk up to (or pinned ones you bring close) get their controls resized now
// and then, not every frame.
void RefreshChrome() {
    static int tick = 0;
    if (++tick % 45) return;
    for (auto &[i, s] : g_screens) {
        if (s.drag != Drag::None) continue;
        const double before = s.chrome;
        ChromeSize(s);
        if (std::fabs(s.chrome - before) > before * 0.08) PlaceChrome(s);
        else s.chrome = before;
    }
}

void SetAbsolute(Screen &s, const Mat &pose) {
    s.pinned = kNone;
    s.pose = pose;
    vr::VROverlay()->SetOverlayTransformAbsolute(s.overlay, vr::TrackingUniverseStanding, &pose);
    PlaceChrome(s);
}

void Pin(Screen &s, vr::TrackedDeviceIndex_t dev, const Mat &rel) {
    s.pinned = dev;
    s.pinRel = rel;
    vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(s.overlay, dev, &s.pinRel);
    PlaceChrome(s);
}

void SetWidth(Screen &s, double metres) {
    s.metres = std::clamp(metres, kMinWidth, 12.0);
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    ApplyCurve(s);  // same radius, so the curvature fraction changes with the width
    PlaceChrome(s);
}

// Curve toward the head: the radius is the head's distance to the screen now.
void ToggleCurve(Screen &s) {
    Mat head, p;
    if (s.curve > 0 || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) || !ScreenPose(s, &p)) {
        s.curve = 0;
    } else {
        const double dx = p.m[0][3] - head.m[0][3], dy = p.m[1][3] - head.m[1][3], dz = p.m[2][3] - head.m[2][3];
        s.curve = std::max(0.5, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    ApplyCurve(s);
    PlaceChrome(s);
}

// ---------------------------------------------------------------- visibility

// Angle in degrees between a panel's front and the direction from it to the head.
double FacingAngle(const Mat &p, const Mat &head) {
    double n[3], to[3] = {head.m[0][3] - p.m[0][3], head.m[1][3] - p.m[1][3], head.m[2][3] - p.m[2][3]};
    Column(p, 2, n);
    const double len = std::sqrt(Dot3(to, to)) + 1e-9;
    return std::acos(std::clamp(Dot3(n, to) / len, -1.0, 1.0)) * 180 / M_PI;
}

// The screens' shared visibility for the mode (before a pinned screen's own facing rule).
// The mode in effect: during a VR game (with g_inGames Hide), "always" becomes "only with
// the dashboard open", so the screens stay out of the game until you open the dashboard.
Mode EffectiveMode() {
    return g_gameRunning && g_inGames == InGames::Hide && g_mode == Mode::Always ? Mode::Dashboard : g_mode;
}

// A VR game starting or stopping (checked twice a second) resets the hide/show switch, whose
// meaning depends on the mode in effect.
void UpdateGame() {
    if (g_tick % 45) return;
    const bool running = vr::VRApplications()->GetCurrentSceneProcessId() != 0;
    if (running == g_gameRunning) return;
    g_gameRunning = running;
    g_manual = false;
    std::printf("%s\n", running ? "a VR game started" : "the VR game ended");
}

bool ModeVisible() {
    if (g_paused) return false;
    switch (EffectiveMode()) {
        case Mode::Always: return !g_manual;
        case Mode::Toggle: return g_manual;
        case Mode::Dashboard: return g_manual || vr::VROverlay()->IsDashboardVisible();
        case Mode::Gesture: {
            if (g_manual) return true;
            // Looking at the chosen controller: it's within the gesture angle of the gaze.
            Mat head, c;
            if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head) ||
                !DevicePose(HandDevice(g_gestureHand.c_str()), &c))
                return false;
            double f[3], to[3] = {c.m[0][3] - head.m[0][3], c.m[1][3] - head.m[1][3], c.m[2][3] - head.m[2][3]};
            Column(head, 2, f);  // the head's +Z points backward
            const double len = std::sqrt(Dot3(to, to)) + 1e-9;
            return std::acos(std::clamp(-Dot3(f, to) / len, -1.0, 1.0)) * 180 / M_PI <= g_gestureAngle;
        }
    }
    return true;
}

// The screen at its alpha; each control dimmer (kChromeIdle) unless a laser is on it or
// it's being dragged.
void ApplyAlpha(const Screen &s) {
    vr::VROverlay()->SetOverlayAlpha(s.overlay, s.alpha);
    for (const auto &[k, sub] : s.subs) vr::VROverlay()->SetOverlayAlpha(sub.overlay, s.alpha);
    const bool active[7] = {s.hover[0] || s.drag == Drag::Move, s.hover[1], s.hover[2] || s.drag == Drag::Roll,
                            s.hover[3] || s.drag == Drag::Resize, s.hover[4], s.hover[5], s.hover[6]};
    const auto controls = s.Controls();
    for (int k = 0; k < 7; ++k)
        if (controls[k] != vr::k_ulOverlayHandleInvalid)
            vr::VROverlay()->SetOverlayAlpha(controls[k], s.alpha * s.controls * (active[k] ? 1.f : kChromeIdle));
}

void SetVisible(Screen &s, bool visible, float alpha) {
    if (visible && std::fabs(alpha - s.alpha) > 0.01f) {
        s.alpha = alpha;
        ApplyAlpha(s);
    }
    if (visible == s.visible) return;
    s.visible = visible;
    if (visible) {
        vr::VROverlay()->ShowOverlay(s.overlay);
        for (const auto &[k, sub] : s.subs) vr::VROverlay()->ShowOverlay(sub.overlay);
        return;
    }
    // Hidden: the controls go at once (UpdateControls brings them back).
    vr::VROverlay()->HideOverlay(s.overlay);
    for (const auto &[k, sub] : s.subs) vr::VROverlay()->HideOverlay(sub.overlay);
    for (auto o : s.Controls())
        if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->HideOverlay(o);
    s.controls = 0, s.controlsUp = false;
}

void UpdateVisibility() {
    const bool shared = ModeVisible();
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    for (auto &[i, s] : g_screens) {
        bool visible = !g_paused && s.shown && (shared || s.drag != Drag::None) && !s.alone;
        // A floating window's panel: while a window floats on it, its output is on, and the
        // window isn't minimized (and once it has a crop).
        if (s.floating) visible = visible && s.floatOn && s.outputOn && !s.minimized && s.cropW > 0;
        float alpha = 1;
        Mat p;
        if (visible && s.pinned != kNone && s.pinned != vr::k_unTrackedDeviceIndex_Hmd && s.drag == Drag::None &&
            haveHead && ScreenPose(s, &p)) {
            // A pinned screen shows while you see its front: fully inside the wrist angle,
            // fading out over the last kFade degrees, gone beyond it (and from behind).
            const double a = FacingAngle(p, head);
            alpha = float(std::clamp((g_wristAngle - a) / kFade, 0.0, 1.0));
            visible = alpha > 0.02f;
        }
        SetVisible(s, visible, alpha);
    }
}

// Attention, for each screen's frame rate (compositor.c gives KWin frame callbacks at a
// rate for each level): focused while you look at the screen (within kFocusAngle of where
// your head points), a laser or the mouse is on it, or it's being carried; in view while
// any of it is within kViewAngle; hidden otherwise, or while it isn't shown. A level stays
// for a moment after its reason goes, so a glance away doesn't make it stutter, and goes up
// at once.
constexpr double kFocusAngle = 12, kViewAngle = 60;  // degrees
constexpr int64_t kFocusLinger = 1500, kViewLinger = 500, kInputFocus = 1500;  // ms

bool RayOnPlane(const Mat &p, const Mat &d, double *x, double *y);

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

// The smallest angle between where the head points and the screen: to the point of its
// rectangle nearest where the head's ray meets its plane, its centre, and its corners. A
// curved screen counts as flat; the angles hardly differ.
double AngleToScreen(const Screen &s, const Mat &p, const Mat &head) {
    const double hw = s.metres / 2, hh = s.heightMetres() / 2;
    double f[3];
    Column(head, 2, f);  // the head's +Z points backward
    auto angleTo = [&](double u, double v) {
        double to[3];
        for (int i = 0; i < 3; ++i) to[i] = p.m[i][3] + u * p.m[i][0] + v * p.m[i][1] - head.m[i][3];
        const double len = std::sqrt(Dot3(to, to)) + 1e-9;
        return std::acos(std::clamp(-Dot3(f, to) / len, -1.0, 1.0)) * 180 / M_PI;
    };
    double best = 180, x, y;
    if (RayOnPlane(p, head, &x, &y)) best = angleTo(std::clamp(x, -hw, hw), std::clamp(y, -hh, hh));
    for (double u : {-hw, 0.0, hw})
        for (double v : {-hh, 0.0, hh}) best = std::min(best, angleTo(u, v));
    return best;
}

void UpdateAttention() {
    const int64_t now = NowMs();
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    for (auto &[i, s] : g_screens) {
        if (!s.visible) {
            s.attention = FT_HIDDEN;
            s.focusUntil = s.viewUntil = 0;
            continue;
        }
        bool focus = s.drag != Drag::None || now - s.inputMs < kInputFocus, view = focus;
        Mat p;
        if (!haveHead) {
            view = true;  // nothing to go by
        } else if (ScreenPose(s, &p)) {
            const double a = AngleToScreen(s, p, head);
            focus = focus || a <= kFocusAngle;
            view = view || a <= kViewAngle;
        }
        if (focus) s.focusUntil = now + kFocusLinger;
        if (view) s.viewUntil = now + kViewLinger;
        s.attention = now < s.focusUntil ? FT_FOCUSED : now < s.viewUntil ? FT_IN_VIEW : FT_HIDDEN;
    }
}


// Controllers' lasers on the screens (see the top): the flag follows the mode and whether a
// VR game runs, and where the mode leaves the controllers to the game, whether one points at
// the panel (UpdateAim).
bool LasersByMode() { return g_lasers == Lasers::Always || (g_lasers == Lasers::OutsideGames && !g_gameRunning); }
long g_keyboardAimUntil = 0;

void UpdateLasers() {
    const bool byMode = LasersByMode();
    for (auto &[i, s] : g_screens) {
        const bool want = byMode || (s.visible && g_tick < s.aimUntil);
        if (s.lasers == want) continue;
        s.lasers = want;
        vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, want);
    }
    if (keyboard::Shown()) keyboard::SetLasers(byMode || g_tick < g_keyboardAimUntil);
}

// The distance from a laser's line to a point ahead of it, or -1 when it's behind.
double RayDistance(const Mat &d, const Mat &c) {
    const double o[3] = {d.m[0][3], d.m[1][3], d.m[2][3]}, dir[3] = {-d.m[0][2], -d.m[1][2], -d.m[2][2]};
    const double v[3] = {c.m[0][3] - o[0], c.m[1][3] - o[1], c.m[2][3] - o[2]};
    const double t = Dot3(v, dir);
    if (t <= 0) return -1;
    const double q[3] = {v[0] - dir[0] * t, v[1] - dir[1] * t, v[2] - dir[2] * t};
    return std::sqrt(Dot3(q, q));
}

// The controls are invisible until a laser is on one of them (SteamVR's hover event) or
// passes very close (within `reach`, about 1.5 times a button's size); they stay
// kControlsLinger ticks after it leaves, and while in use.
void UpdateControls() {
    std::vector<Mat> lasers;
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        Mat d;
        if (vr::VRSystem()->GetTrackedDeviceClass(i) == vr::TrackedDeviceClass_Controller && LaserPose(i, &d))
            lasers.push_back(d);
    }
    for (auto &[i, s] : g_screens) {
        Mat p;
        if (s.visible && ScreenPose(s, &p)) {
            // Points along the bar and at each button and the tab; a laser passing within
            // `reach` of one of them is close.
            std::vector<Mat> spots;
            const auto offsets = ControlOffsets(s);
            for (double f : {-0.5, -0.25, 0.0, 0.25, 0.5})
                spots.push_back(Mul(p, Mul(offsets[0], Translation(f * s.chrome, 0, 0))));
            const auto controls = s.Controls();
            for (int k = 1; k < 7; ++k)
                if (controls[k] != vr::k_ulOverlayHandleInvalid) spots.push_back(Mul(p, offsets[k]));
            const double reach = std::max(s.grip * 1.5, s.chrome * 0.12);
            for (const Mat &d : lasers) {
                bool close = false;
                for (const Mat &c : spots) {
                    const double r = RayDistance(d, c);
                    if (r >= 0 && r <= reach) close = true;
                }
                if (close) {
                    s.nearUntil = g_tick + kControlsLinger;
                    break;
                }
            }
        }
        const bool inUse = s.drag != Drag::None || std::any_of(std::begin(s.hover), std::end(s.hover), [](bool h) { return h; });
        const bool want = s.visible && (inUse || g_tick < s.nearUntil);
        // The controls stay shown while their screen is, just fully transparent when not
        // wanted: SteamVR's laser still hits them, and the hover event brings them in, for
        // any device's laser, whatever its shape.
        if (s.visible && !s.controlsUp) {
            for (auto o : s.Controls())
                if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->ShowOverlay(o);
            s.controlsUp = true;
            ApplyAlpha(s);
        }
        const float before = s.controls;
        s.controls = std::clamp(s.controls + (want ? 0.2f : -0.1f), 0.f, 1.f);
        if (s.controls != before) ApplyAlpha(s);
    }
}

// ---------------------------------------------------------------- moving, resizing, pinning

// To ft-floatd (@frametop_float), for floating windows: dock, close, resize. From an unbound
// socket, so its replies go nowhere.
void SendFloat(const std::string &msg) {
    static const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const char name[] = "frametop_float";
    std::memcpy(addr.sun_path + 1, name, sizeof name - 1);
    sendto(fd, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&addr),
           socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1));
    if (msg.rfind("resize ", 0) != 0)  // an edge drag sends many resizes a second
        std::printf("to ft-floatd: %s\n", msg.c_str());
}

// To the pointer helper (@ft_pointer_helper), from an unbound socket.
void SendPointer(const std::string &msg) {
    static const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const char name[] = "ft_pointer_helper";
    std::memcpy(addr.sun_path + 1, name, sizeof name - 1);
    sendto(fd, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&addr),
           socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1));
}

// A new panel: the pointer helper reads SteamVR's list of panels only every 20 s, so it's told
// at once ("overlay <key>"), or the mouse couldn't click a menu until then.
void AnnounceOverlay(const char *key) { SendPointer(std::string("overlay ") + key); }

// Where a device's ray meets the screen's plane, in the screen's x (right) and y (up),
// metres from its centre.
bool RayOnPlane(const Mat &p, const Mat &d, double *x, double *y) {
    const double o[3] = {d.m[0][3], d.m[1][3], d.m[2][3]}, dir[3] = {-d.m[0][2], -d.m[1][2], -d.m[2][2]};
    const double c[3] = {p.m[0][3], p.m[1][3], p.m[2][3]};
    double n[3], ax[3], ay[3];
    Column(p, 2, n), Column(p, 0, ax), Column(p, 1, ay);
    const double denom = Dot3(dir, n);
    if (std::fabs(denom) < 1e-4) return false;
    const double co[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    const double t = Dot3(co, n) / denom;
    if (t <= 0) return false;
    const double rel[3] = {o[0] + dir[0] * t - c[0], o[1] + dir[1] * t - c[1], o[2] + dir[2] * t - c[2]};
    *x = Dot3(rel, ax), *y = Dot3(rel, ay);
    return true;
}
bool RayOnScreen(const Screen &s, const Mat &d, double *x, double *y) {
    Mat p;
    return ScreenPose(s, &p) && RayOnPlane(p, d, x, y);
}

// Roll: a rotation about the screen's own front axis (counterclockwise as you see it).
Mat RollZ(double rad) {
    Mat m = Identity();
    m.m[0][0] = m.m[1][1] = float(std::cos(rad));
    m.m[1][0] = float(std::sin(rad)), m.m[0][1] = float(-std::sin(rad));
    return m;
}

// Roll the screen to `rad` from its pose at the press, snapping level within kRollSnap.
void ApplyRoll(Screen &s, double rad) {
    const bool pinned = s.pinned != kNone;
    Mat c = Identity();
    if (pinned && !DevicePose(s.pinned, &c)) return;
    const Mat base = pinned ? Mul(c, s.rollFrom) : s.rollFrom;
    const Mat p = Mul(base, RollZ(rad));
    const double tilt = std::asin(std::clamp(double(p.m[1][0]), -1.0, 1.0));  // the right edge's slope
    if (std::fabs(tilt) < kRollSnap * M_PI / 180) rad -= tilt;
    if (pinned) Pin(s, s.pinned, Mul(s.rollFrom, RollZ(rad)));
    else SetAbsolute(s, Mul(base, RollZ(rad)));
}

// The laser's angle around the screen's centre, in the frame of its pose at the press.
bool RollLaserAngle(const Screen &s, const Mat &d, double *rad) {
    Mat c = Identity();
    if (s.pinned != kNone && !DevicePose(s.pinned, &c)) return false;
    const Mat base = s.pinned != kNone ? Mul(c, s.rollFrom) : s.rollFrom;
    double hx, hy;
    if (!RayOnPlane(base, d, &hx, &hy)) return false;
    *rad = std::atan2(hy, hx);
    return true;
}

// The laser while moving a screen: from the carrying device to the bar.
void Laser(const Screen &s, const Mat &d, const Mat &p, double a[3], double b[3]) {
    const Mat bar = Mul(p, BarOffset(s));
    for (int k = 0; k < 3; ++k) a[k] = d.m[k][3], b[k] = bar.m[k][3];
}

// The point q on the segment a-b closest to pt, and its distance.
double SegmentClosest(const double pt[3], const double a[3], const double b[3], double q[3]) {
    const double ab[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, ap[3] = {pt[0] - a[0], pt[1] - a[1], pt[2] - a[2]};
    const double t = std::clamp(Dot3(ap, ab) / (Dot3(ab, ab) + 1e-12), 0.0, 1.0);
    for (int k = 0; k < 3; ++k) q[k] = a[k] + ab[k] * t;
    const double v[3] = {q[0] - pt[0], q[1] - pt[1], q[2] - pt[2]};
    return std::sqrt(Dot3(v, v));
}
double LaserDistance(const Screen &s, const Mat &d, const Mat &p, vr::TrackedDeviceIndex_t dev, double q[3]) {
    Mat c;
    if (!DevicePose(dev, &c)) return 1e9;
    double a[3], b[3];
    Laser(s, d, p, a, b);
    const double pt[3] = {c.m[0][3], c.m[1][3], c.m[2][3]};
    return SegmentClosest(pt, a, b, q);
}

// The hand controller (not the carrying device) whose ring the laser is in, or kNone.
vr::TrackedDeviceIndex_t WristOnLaser(const Screen &s, const Mat &d, const Mat &p) {
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        double q[3];
        if (i != s.dragDevice && IsHandController(i) && LaserDistance(s, d, p, i, q) <= kWristZone) return i;
    }
    return kNone;
}

void StartDrag(Screen &s, Drag mode, vr::TrackedDeviceIndex_t dev) {
    Mat d, p;
    if (dev == kNone || !DevicePose(dev, &d) || !ScreenPose(s, &p)) return;
    s.pinTarget = kNone;
    if (s.pinned != kNone && mode == Drag::Move) {
        // Carried freely; let go, it goes back on the same wrist (unless disarmed).
        s.pinTarget = s.pinned;
        SetAbsolute(s, p);
    }
    s.drag = mode;
    s.dragDevice = dev;
    s.dragRel = Mul(Inverse(d), p);
    // Already in a ring when grabbed: that doesn't count as crossing it.
    s.onWrist = mode == Drag::Move ? WristOnLaser(s, d, p) : kNone;
    LightBar(s, s.pinTarget != kNone);
    if (mode == Drag::Resize) {
        double hx, hy;
        Mat l;
        if (LaserPose(dev, &l) && RayOnScreen(s, l, &hx, &hy)) s.grabX = hx - s.metres / 2, s.grabY = hy + s.heightMetres() / 2;
        else s.grabX = s.grabY = 0;
    }
    if (mode == Drag::Roll) {
        s.rollFrom = s.pinned != kNone ? s.pinRel : p;
        Mat l;
        if (!LaserPose(dev, &l) || !RollLaserAngle(s, l, &s.rollAngle)) s.drag = Drag::None, s.dragDevice = kNone;
    }
    ApplyAlpha(s);
}

// Stop moving where it is (a command took over).
void EndDrag(Screen &s) {
    s.drag = Drag::None;
    s.dragDevice = kNone;
    s.pinTarget = s.onWrist = kNone;
    LightBar(s, false);
    ApplyAlpha(s);
}

// Run `ft-layout <cmd>` in the background, logging to the host's runtime directory
// (XDG_RUNTIME_DIR, not the nested desktop's; else /tmp with O_NOFOLLOW: a symlink there
// must not be followed). The desktop start truncates the same log.
void RunLayout(const char *cmd) {
    char exe[PATH_MAX];
    if (!realpath("/proc/self/exe", exe)) return;
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string logname = std::string(runtime && *runtime ? runtime : "/tmp") + "/frametop-layout.log";
    std::string layout(exe);  // <repo>/screens/build/ft-screens -> <repo>/layout/ft-layout
    for (int up = 0; up < 3 && layout.rfind('/') != std::string::npos; ++up) layout.resize(layout.rfind('/'));
    layout += "/layout/ft-layout";
    posix_spawn_file_actions_t io;
    posix_spawn_file_actions_init(&io);
    posix_spawn_file_actions_addopen(&io, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&io, 1, logname.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW, 0644);
    posix_spawn_file_actions_adddup2(&io, 1, 2);
    std::string arg(cmd);
    char *argv[] = {layout.data(), arg.data(), nullptr};
    pid_t pid;  // reaped by the compositor's SIGCHLD handler
    // A log that can't be opened (a symlink or a directory in its place) fails the spawn too.
    const int rc = posix_spawn(&pid, layout.c_str(), &io, nullptr, argv, environ);
    if (rc != 0)
        std::printf("can't run %s (log %s): %s\n", layout.c_str(), logname.c_str(), std::strerror(rc));
    posix_spawn_file_actions_destroy(&io);
}

// KWin's outputs follow where the screens are, so the pointer and dragged windows cross
// to the screen you see next to this one: `ft-layout scale` runs once a move has settled.
long g_arrangeAt = -1;  // g_tick to run it at, -1 = not pending

void ArrangeDesktopSoon() { g_arrangeAt = g_tick + 45; }  // about half a second

void UpdateArrange() {
    if (g_arrangeAt < 0 || g_tick < g_arrangeAt) return;
    g_arrangeAt = -1;
    RunLayout("scale");
}

// Let go: pin to the armed wrist, as the screen is now.
void FinishDrag(Screen &s, int index) {
    const bool moved = s.drag == Drag::Move;
    const vr::TrackedDeviceIndex_t target = s.pinTarget;
    EndDrag(s);
    Mat c, p;
    if (!moved) return;
    if (!s.floating) ArrangeDesktopSoon();
    if (target != kNone && DevicePose(target, &c) && ScreenPose(s, &p)) {
        Pin(s, target, Mul(Inverse(c), p));
        if (target == vr::k_unTrackedDeviceIndex_Hmd) std::printf("screen %d: pinned to the head\n", index + 1);
        else std::printf("screen %d: pinned to the %s controller\n", index + 1, HandName(target));
    }
}

// A button release on any of our panels ends that device's drags (it may be over another
// screen by then).
void EndDragsBy(vr::TrackedDeviceIndex_t dev) {
    keyboard::EndDragBy(dev);
    for (auto &[index, s] : g_screens)
        if (s.drag != Drag::None && s.dragDevice == dev) FinishDrag(s, index);
}

// While moving: the laser entering a controller's ring flips whether the screen pins to
// it when let go (so sweeping across arms it, sweeping back disarms it).
void CheckWristAim(Screen &s, const Mat &d, const Mat &p) {
    double q[3];
    if (s.onWrist != kNone && LaserDistance(s, d, p, s.onWrist, q) > kWristLeave) s.onWrist = kNone;
    if (s.onWrist == kNone) {
        s.onWrist = WristOnLaser(s, d, p);
        if (s.onWrist != kNone) s.pinTarget = s.pinTarget == s.onWrist ? kNone : s.onWrist;
    }
    LightBar(s, s.pinTarget != kNone);
}

// Show the rings and dots for the screen being carried (hide them otherwise).
void UpdateGuides() {
    const Screen *carried = nullptr;
    Mat d, p, head;
    for (auto &[i, s] : g_screens)
        if (s.drag == Drag::Move && DevicePose(s.dragDevice, &d) && ScreenPose(s, &p)) {
            carried = &s;
            break;
        }
    if (!carried || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        for (auto &[dev, g] : g_guides) g.ring.Show(false), g.dot.Show(false);
        return;
    }
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        Mat c;
        const bool want = i != carried->dragDevice && IsHandController(i) && DevicePose(i, &c);
        if (!want) {
            auto it = g_guides.find(i);
            if (it != g_guides.end()) it->second.ring.Show(false), it->second.dot.Show(false);
            continue;
        }
        Guide &g = GuideFor(i);
        const double pt[3] = {c.m[0][3], c.m[1][3], c.m[2][3]};
        Mat m = FacingPose(pt, head);
        vr::VROverlay()->SetOverlayTransformAbsolute(g.ring.overlay, vr::TrackingUniverseStanding, &m);
        g.ring.Light(carried->pinTarget == i, RingTexture(carried->pinTarget == i), 128);
        g.ring.Show(true);
        double q[3];
        const double dist = LaserDistance(*carried, d, p, i, q);
        if (dist <= kDotRange) {
            m = FacingPose(q, head);
            vr::VROverlay()->SetOverlayTransformAbsolute(g.dot.overlay, vr::TrackingUniverseStanding, &m);
            g.dot.Light(dist <= kWristZone, DotTexture(dist <= kWristZone), 32);
        }
        g.dot.Show(dist <= kDotRange);
    }
}

void UpdateDrag(Screen &s, int index) {
    Mat d;
    if (!DevicePose(s.dragDevice, &d)) return;
    if (s.drag == Drag::Move) {
        const Mat p = Mul(d, s.dragRel);
        SetAbsolute(s, p);
        CheckWristAim(s, d, p);
        return;
    }
    if (s.drag == Drag::Roll) {
        // Like turning a knob: the screen turns as far as the laser has gone around its centre.
        double a;
        Mat l;
        if (!LaserPose(s.dragDevice, &l) || !RollLaserAngle(s, l, &a)) return;
        ApplyRoll(s, std::remainder(a - s.rollAngle, 2 * M_PI));
        return;
    }
    // Resize: the corner follows the ray along the screen's diagonal (so it shrinks and
    // grows from any direction), keeping where on the handle it was grabbed.
    double hx, hy;
    Mat l;
    if (!LaserPose(s.dragDevice, &l) || !RayOnScreen(s, l, &hx, &hy)) return;
    if (s.floating) {
        // A floating window: the corner goes where the laser is, in both directions, and the
        // window gets that many pixels at the same density (ft-floatd resizes it, and the
        // new crop comes back as "float", with the top left corner kept where it is).
        if (s.mpp <= 0 || g_tick - s.resizeSent < 4) return;  // about 20 a second
        const double left = -s.metres / 2, top = s.heightMetres() / 2;
        const int w = std::max(320, int(std::lround((hx - s.grabX - left) / s.mpp)));
        const int h = std::max(200, int(std::lround((top - (hy - s.grabY)) / s.mpp)));
        if (w == s.resizeW && h == s.resizeH) return;
        s.resizeW = w, s.resizeH = h, s.resizeSent = g_tick;
        SendFloat("resize " + std::to_string(index + 1) + " " + std::to_string(w) + " " + std::to_string(h));
        return;
    }
    const double a = s.width > 0 ? double(s.height) / s.width : 9.0 / 16;
    const double cx = hx - s.grabX, cy = hy - s.grabY;  // where the corner should be
    SetWidth(s, 2 * (cx - a * cy) / (1 + a * a));
}

// Scroll while moving: push the screen away (up) or pull it closer, along the line from
// the head (not from the carrying device: the 3D mouse's device sits just in front of the
// bar, below the screen's centre, so that line points mostly up).
void Push(Screen &s, double notches) {
    Mat d, head;
    if (!DevicePose(s.dragDevice, &d) || !DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) return;
    Mat p = Mul(d, s.dragRel);
    const double to[3] = {p.m[0][3] - head.m[0][3], p.m[1][3] - head.m[1][3], p.m[2][3] - head.m[2][3]};
    const double len = std::sqrt(Dot3(to, to));
    const double next = std::clamp(len * (1 + 0.08 * notches), 0.3, 10.0);
    for (int k = 0; k < 3; ++k) p.m[k][3] = float(head.m[k][3] + to[k] / (len + 1e-9) * next);
    s.dragRel = Mul(Inverse(d), p);
}

// ---------------------------------------------------------------- floating windows

// Show the window's rectangle of the buffer. Texture bounds are fractions of the buffer, v
// from the top. SteamVR reports mouse positions in the whole texture (the bounds applied), so
// the mouse scale is the buffer's size, as on a screen (see ToBuffer).
void CropOverlay(vr::VROverlayHandle_t o, const Screen &s, int x, int y, int w, int h) {
    if (s.width <= 0 || s.height <= 0 || w <= 0 || h <= 0) return;
    vr::VRTextureBounds_t b = {float(x) / s.width, float(y) / s.height, float(x + w) / s.width,
                               float(y + h) / s.height};
    vr::VROverlay()->SetOverlayTextureBounds(o, &b);
    vr::HmdVector2_t scale = {float(s.width), float(s.height)};
    vr::VROverlay()->SetOverlayMouseScale(o, &scale);
}

void ApplyCrop(Screen &s) {
    CropOverlay(s.overlay, s, s.cropX, s.cropY, s.cropW, s.cropH);
    for (const auto &[k, sub] : s.subs) CropOverlay(sub.overlay, s, sub.x, sub.y, sub.w, sub.h);
}

// ft-floatd's "float": the window's rectangle, its title bar, and the density. The panel's
// top left corner stays where it is when the window changes size.
void SetFloat(Screen &s, double mpp, int x, int y, int w, int h, int title) {
    const bool first = !s.floatOn || s.cropW <= 0;
    const double oldW = s.metres, oldH = s.heightMetres();
    s.floatOn = true;
    s.mpp = mpp;
    s.cropX = x, s.cropY = y, s.cropW = w, s.cropH = h, s.titleH = title;
    s.metres = w * mpp;
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    ApplyCurve(s);
    ApplyCrop(s);
    const double dx = (s.metres - oldW) / 2, dy = -(s.heightMetres() - oldH) / 2;
    if (!first && (std::fabs(dx) > 1e-6 || std::fabs(dy) > 1e-6)) {
        if (s.pinned != kNone) Pin(s, s.pinned, Mul(s.pinRel, Translation(dx, dy, 0)));
        else SetAbsolute(s, Mul(s.pose, Translation(dx, dy, 0)));
    } else {
        PlaceChrome(s);
    }
}

void Unfloat(Screen &s) {
    if (s.drag != Drag::None) EndDrag(s);
    for (auto &[k, sub] : s.subs) vr::VROverlay()->DestroyOverlay(sub.overlay);
    s.subs.clear();
    s.floatOn = s.minimized = s.titleCarry = false;
    s.cropW = s.cropH = 0;
    s.resizeW = s.resizeH = 0;
}

// A popup or dialog (number k) at x, y, w, h in the buffer; w = 0 takes it away.
void SetSub(Screen &s, int index, int k, int x, int y, int w, int h) {
    auto it = s.subs.find(k);
    if (w <= 0 || h <= 0) {
        if (it != s.subs.end()) {
            vr::VROverlay()->DestroyOverlay(it->second.overlay);
            s.subs.erase(it);
        }
        return;
    }
    if (it == s.subs.end()) {
        Sub sub;
        char key[80], name[64];
        std::snprintf(key, sizeof key, "frametop.float.%d.sub.%d", index + 1, k);
        std::snprintf(name, sizeof name, "Floating window menu %d", k);
        if (vr::VROverlay()->CreateOverlay(key, name, &sub.overlay) != vr::VROverlayError_None) return;
        vr::VROverlay()->SetOverlayInputMethod(sub.overlay, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayFlag(sub.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
        vr::VROverlay()->SetOverlayFlag(sub.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
        vr::VROverlay()->SetOverlayFlag(sub.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, s.lasers);
        vr::VROverlay()->SetOverlaySortOrder(sub.overlay, 5);
        AnnounceOverlay(key);
        it = s.subs.emplace(k, sub).first;
        if (s.shown) {
            auto imp = g_imports.find(s.shown);
            if (imp != g_imports.end()) {
                vr::SharedTextureHandle_t handle = imp->second;
                vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
                vr::VROverlay()->SetOverlayTexture(sub.overlay, &tex);
            }
        }
        vr::VROverlay()->SetOverlayAlpha(sub.overlay, s.alpha);
        if (s.visible) vr::VROverlay()->ShowOverlay(sub.overlay);
    }
    it->second.x = x, it->second.y = y, it->second.w = w, it->second.h = h;
    CropOverlay(it->second.overlay, s, x, y, w, h);
    PlaceSubs(s);
}

// ---------------------------------------------------------------- the catcher

// A button pressed on a screen belongs to KWin until it comes up, wherever the laser is by
// then: a window move or a drag and drop can end between panels. SteamVR sends the release
// only to an overlay under the laser, so while the pressing laser is on none of our panels,
// an invisible catcher sits on it, at the distance where it last met one, and a release
// there goes to KWin at the pointer's last spot. While a button is held, the laser leaving
// a screen doesn't take KWin's pointer away either, as with a real mouse; crossing onto
// another screen still moves it there.
struct Press {
    uint32_t buttons = 0;                     // held, as bits (1 << (BTN_* - BTN_LEFT))
    vr::TrackedDeviceIndex_t device = kNone;  // the laser that pressed them
    int screen = -1;                          // where KWin's pointer is: the last screen the laser was on
    double x = 0, y = 0;                      // ...and where on it, in buffer pixels
    double distance = 1;                      // from the laser's start to the last panel it met
    long upAt = -1;                           // the pointer helper saw left come up: release it at this tick
    int64_t idleSince = -1;                   // the pressing controller has held nothing since (ms, ReleaseStuck)
};
Press g_press;
vr::VROverlayHandle_t g_catcher = vr::k_ulOverlayHandleInvalid;
bool g_catcherShown = false;

uint32_t ButtonBit(uint32_t linuxButton) { return 1u << (linuxButton - BTN_LEFT); }

void PressDown(vr::TrackedDeviceIndex_t dev, uint32_t button, int screen, double x, double y) {
    if (!g_press.buttons) g_press.device = dev;
    g_press.buttons |= ButtonBit(button);
    g_press.screen = screen, g_press.x = x, g_press.y = y;
}

// A button came up somewhere that isn't a screen (the catcher, a control, or the helper's
// word): release it in KWin where its pointer is, and once nothing is held, the laser is
// off the screens, so KWin's pointer leaves.
void ReleaseAway(uint32_t button, void (*handle)(const struct ft_event *, void *), void *data) {
    if (!(g_press.buttons & ButtonBit(button))) return;
    g_press.buttons &= ~ButtonBit(button);
    if (!g_press.buttons) g_press.upAt = -1;
    if (g_press.screen < 0) return;
    ft_event e{};
    e.type = FT_BUTTON;
    e.screen = g_press.screen;
    e.button = button;
    e.pressed = false;
    e.x = g_press.x, e.y = g_press.y;
    handle(&e, data);
    std::printf("caught a release off the screens (button %u)\n", button);
    if (g_press.buttons) return;
    e = ft_event{};
    e.type = FT_LEAVE;
    e.screen = g_press.screen;
    handle(&e, data);
}

// Whether a hand controller holds anything (a button down): 1 yes, 0 no, -1 unknown. SteamVR
// answers overlay apps only while a VR game runs (checked 2026-10-04); outside games it can't
// say. The Frame controller's axes have no types, so the buttons are all there is.
int ControllerHolds(vr::TrackedDeviceIndex_t dev) {
    vr::VRControllerState_t st{};
    if (!vr::VRSystem()->GetControllerState(dev, &st, sizeof st)) return -1;
    return st.ulButtonPressed ? 1 : 0;
}

// Releases SteamVR never sends. Pausing, or hiding the screen a button went down on, takes the
// laser off it mid-click (the pause gesture's second thumbstick click does that), and SteamVR's
// laser mouse then forgets the button: no release comes, the catcher stayed up, and the game
// never got its controllers back (2026-10-04). So a held button is released here when it has
// no visible screen to come up on, or when its hand controller has held nothing for kStuckMs
// (only known during VR games, where the lost release took the game's controllers).
// The pointer helper's virtual controller has its own word for that ("up").
constexpr int64_t kStuckMs = 1000;
void ReleaseStuck(void (*handle)(const struct ft_event *, void *), void *data) {
    if (!g_press.buttons) {
        g_press.idleSince = -1;
        return;
    }
    const char *why = nullptr;
    const auto it = g_screens.find(g_press.screen);
    if (g_paused) why = "paused";
    else if (g_press.screen >= 0 && (it == g_screens.end() || !it->second.visible)) why = "its screen hid";
    else if (IsHandController(g_press.device)) {
        const int64_t now = NowMs();
        if (ControllerHolds(g_press.device) != 0) g_press.idleSince = -1;
        else if (g_press.idleSince < 0) g_press.idleSince = now;
        else if (now - g_press.idleSince >= kStuckMs) why = "the controller holds nothing";
    }
    if (!why) return;
    std::printf("a held button can't come up on a screen (%s): released\n", why);
    const vr::TrackedDeviceIndex_t dev = g_press.device;
    for (uint32_t b = 0; b < 32; ++b)
        if (g_press.buttons & (1u << b)) ReleaseAway(BTN_LEFT + b, handle, data);
    g_press.buttons = 0, g_press.upAt = -1, g_press.idleSince = -1;
    EndDragsBy(dev);
}

void ShowCatcher(bool on) {
    if (on == g_catcherShown || g_catcher == vr::k_ulOverlayHandleInvalid) return;
    g_catcherShown = on;
    if (on) vr::VROverlay()->ShowOverlay(g_catcher);
    else vr::VROverlay()->HideOverlay(g_catcher);
}

// Every tick: while a button is held, find what the pressing laser is on. On one of our
// panels or controls, note how far away; on none, put the catcher across it there.
void UpdateCatcher() {
    Mat l;
    if (!g_press.buttons || g_catcher == vr::k_ulOverlayHandleInvalid || !LaserPose(g_press.device, &l)) {
        ShowCatcher(false);
        return;
    }
    vr::VROverlayIntersectionParams_t params{};
    params.eOrigin = vr::TrackingUniverseStanding;
    for (int k = 0; k < 3; ++k) params.vSource.v[k] = l.m[k][3], params.vDirection.v[k] = -l.m[k][2];
    for (auto &[i, s] : g_screens) {
        if (!s.visible) continue;
        const auto all = s.All();  // one copy: two calls give two temporaries, not one range
        std::vector<vr::VROverlayHandle_t> parts(all.begin(), all.end());
        for (const auto &[k, sub] : s.subs) parts.push_back(sub.overlay);
        for (auto o : parts) {
            vr::VROverlayIntersectionResults_t hit;
            if (o != vr::k_ulOverlayHandleInvalid && vr::VROverlay()->ComputeOverlayIntersection(o, &params, &hit)) {
                g_press.distance = std::max(0.05, double(hit.fDistance));
                ShowCatcher(false);
                return;
            }
        }
    }
    const double d = g_press.distance;
    const double pt[3] = {l.m[0][3] - l.m[0][2] * d, l.m[1][3] - l.m[1][2] * d, l.m[2][3] - l.m[2][2] * d};
    const Mat m = FacingPose(pt, l);  // across the laser, facing its start
    vr::VROverlay()->SetOverlayTransformAbsolute(g_catcher, vr::TrackingUniverseStanding, &m);
    vr::VROverlay()->SetOverlayWidthInMeters(g_catcher, float(std::max(0.5, 2 * d)));
    ShowCatcher(true);
}

Screen *Find(int one_based) {
    auto it = g_screens.find(one_based - 1);
    return it == g_screens.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------- the lazy susan
// "spin next|prev|<degrees>": the panels in the room (screens and floating windows, not
// pinned ones) turn together about a vertical axis through your head, so the next panel to
// your right (next) or left (prev) glides to straight ahead, or the ring turns by that many
// degrees (positive turns it left, like next). Their arrangement stays as it is: the room
// turns instead of you. A spin that arrives during one adds to it, from where the panels are
// headed, so quick taps carry on smoothly. Grabbing a panel, or a command that places it,
// takes it out of the spin where it is. The 3D mouse's pointer goes to straight ahead.
constexpr double kSpinSeconds = 0.3;  // how long a spin takes
constexpr double kSpinAhead = 8;      // degrees: a panel this near straight ahead is the current one
constexpr double kSpinFocus = 30;     // degrees: when a spin settles, the panel this near ahead gets typing

struct Spin {
    bool on = false;
    double cx = 0, cz = 0;    // the axis
    double from = 0, to = 0;  // radians, turned from the poses in base (positive: to the left)
    Clock::time_point start;
    std::map<int, Mat> base;  // index -> its pose before the spin
    int front = -1;           // settled: this panel came to the front (ft_vr_poll reports it)
} g_spin;

// p turned a radians about the vertical axis through (cx, cz); positive turns it to the left.
Mat Turned(const Mat &p, double a, double cx, double cz) {
    const double c = std::cos(a), s = std::sin(a);
    Mat r = Identity();
    r.m[0][0] = float(c), r.m[0][2] = float(s);
    r.m[2][0] = float(-s), r.m[2][2] = float(c);
    r.m[0][3] = float(cx - c * cx - s * cz);
    r.m[2][3] = float(cz + s * cx - c * cz);
    return Mul(r, p);
}

bool Spinnable(const Screen &s) { return s.pinned == kNone && (!s.floating || s.floatOn) && s.drag == Drag::None; }

double SpinNow() {
    if (!g_spin.on) return g_spin.to;
    const double t = std::min(1.0, std::chrono::duration<double>(Clock::now() - g_spin.start).count() / kSpinSeconds);
    return g_spin.from + (g_spin.to - g_spin.from) * t * t * (3 - 2 * t);
}

void UpdateSpin() {
    if (!g_spin.on) return;
    const double a = SpinNow();
    const bool done = Clock::now() - g_spin.start >= std::chrono::duration<double>(kSpinSeconds);
    for (auto it = g_spin.base.begin(); it != g_spin.base.end();) {
        auto s = g_screens.find(it->first);
        if (s == g_screens.end() || !Spinnable(s->second)) {
            it = g_spin.base.erase(it);  // grabbed, pinned, or gone: it stays where it is now
            continue;
        }
        SetAbsolute(s->second, Turned(it->second, a, g_spin.cx, g_spin.cz));
        ++it;
    }
    if (done) {
        // The panel now nearest straight ahead (of where you face) gets typing and the active window.
        Mat head;
        double nearest = kSpinFocus;
        if (DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
            for (const auto &[i, p] : g_spin.base) {
                const auto it = g_screens.find(i);
                Mat q;
                if (it == g_screens.end() || !it->second.visible || !ScreenPose(it->second, &q)) continue;
                double f[3] = {-head.m[0][2], 0, -head.m[2][2]};
                double d[3] = {q.m[0][3] - head.m[0][3], 0, q.m[2][3] - head.m[2][3]};
                const double fl = std::sqrt(Dot3(f, f)), dl = std::sqrt(Dot3(d, d));
                if (fl < 1e-6 || dl < 1e-6) continue;
                const double a = std::acos(std::clamp(Dot3(f, d) / (fl * dl), -1.0, 1.0)) * 180 / M_PI;
                if (a < nearest) nearest = a, g_spin.front = i;
            }
        }
        g_spin.on = false;
        g_spin.base.clear();
        ArrangeDesktopSoon();  // KWin's outputs follow where the screens are now
    }
}

void SpinCommand(const char *arg, char *reply, int size) {
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head))
        return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
    if (g_spin.on) {
        g_spin.from = SpinNow();  // carry on from where the panels are now
    } else {
        g_spin.base.clear();
        for (auto &[i, s] : g_screens) {
            Mat p;
            if (Spinnable(s) && ScreenPose(s, &p)) g_spin.base[i] = p;
        }
        g_spin.cx = head.m[0][3], g_spin.cz = head.m[2][3];
        g_spin.from = g_spin.to = 0;
    }
    if (g_spin.base.empty()) return (void)std::snprintf(reply, size, "error nothing to spin");
    double turn;  // degrees, positive to the left
    const bool next = !std::strcmp(arg, "next");
    if (next || !std::strcmp(arg, "prev")) {
        // Each visible panel's bearing from where you face, to the right positive, as it will
        // be when the spin so far ends; the nearest one past straight ahead comes to the front.
        double fx = -head.m[0][2], fz = -head.m[2][2];
        const double n = std::sqrt(fx * fx + fz * fz) + 1e-9;
        fx /= n, fz /= n;
        const double rx = -fz, rz = fx;
        double best = 0;
        bool found = false;
        for (const auto &[i, p] : g_spin.base) {
            const auto s = g_screens.find(i);
            if (s == g_screens.end() || !s->second.visible) continue;
            const Mat q = Turned(p, g_spin.to, g_spin.cx, g_spin.cz);
            const double dx = q.m[0][3] - head.m[0][3], dz = q.m[2][3] - head.m[2][3];
            double a = std::atan2(dx * rx + dz * rz, dx * fx + dz * fz) * 180 / M_PI;
            if (!next) a = -a;
            if (a <= kSpinAhead) a += 360;
            if (!found || a < best) best = a, found = true;
        }
        if (!found || best >= 360 - kSpinAhead) {
            if (!g_spin.on) g_spin.base.clear();
            return (void)std::snprintf(reply, size, "ok 0 (no other panel)");
        }
        if (best > 180) best -= 360;  // the short way round
        turn = next ? best : -best;
    } else {
        char *end;
        turn = std::strtod(arg, &end);
        if (end == arg || *end) return (void)std::snprintf(reply, size, "error spin next|prev|<degrees>");
    }
    g_spin.to += turn * M_PI / 180;
    g_spin.start = Clock::now();
    g_spin.on = true;
    SendPointer("recenter");  // the 3D mouse's pointer stays in front of you, on what comes there
    std::snprintf(reply, size, "ok %.1f", turn);
}

uint32_t LinuxButton(uint32_t vrButton) {
    switch (vrButton) {
        case vr::VRMouseButton_Right: return BTN_RIGHT;
        case vr::VRMouseButton_Middle: return BTN_MIDDLE;
        default: return BTN_LEFT;
    }
}

// Any of the holding laser's buttons coming up on one of our controls or the catcher.
void ReleaseAwayBy(vr::TrackedDeviceIndex_t dev, uint32_t vrButton, void (*handle)(const struct ft_event *, void *),
                   void *data) {
    if (g_press.buttons && dev == g_press.device) ReleaseAway(LinuxButton(vrButton), handle, data);
}

// Where a laser meets a panel's surface, in the panel's u (metres along it from the centre,
// along the arc when curved) and v (up). OpenVR curves a screen into a cylinder toward its
// front, centred `curve` metres in front of it (see OnSurface).
bool RayOnSurface(const Screen &s, const Mat &p, const Mat &laser, double *u, double *v) {
    const Mat inv = Inverse(p);
    const double o[3] = {inv.m[0][0] * laser.m[0][3] + inv.m[0][1] * laser.m[1][3] + inv.m[0][2] * laser.m[2][3] + inv.m[0][3],
                         inv.m[1][0] * laser.m[0][3] + inv.m[1][1] * laser.m[1][3] + inv.m[1][2] * laser.m[2][3] + inv.m[1][3],
                         inv.m[2][0] * laser.m[0][3] + inv.m[2][1] * laser.m[1][3] + inv.m[2][2] * laser.m[2][3] + inv.m[2][3]};
    double d[3];
    for (int i = 0; i < 3; ++i) d[i] = -(inv.m[i][0] * laser.m[0][2] + inv.m[i][1] * laser.m[1][2] + inv.m[i][2] * laser.m[2][2]);
    if (s.curve <= 0) {
        if (std::fabs(d[2]) < 1e-6) return false;
        const double t = -o[2] / d[2];
        if (t <= 0) return false;
        *u = o[0] + d[0] * t, *v = o[1] + d[1] * t;
        return true;
    }
    // x^2 + (z - r)^2 = r^2, on the screen's side of the axis (z < r).
    const double r = s.curve, oz = o[2] - r;
    const double a = d[0] * d[0] + d[2] * d[2], b = 2 * (o[0] * d[0] + oz * d[2]), c = o[0] * o[0] + oz * oz - r * r;
    const double disc = b * b - 4 * a * c;
    if (a < 1e-9 || disc < 0) return false;
    for (double t : {(-b - std::sqrt(disc)) / (2 * a), (-b + std::sqrt(disc)) / (2 * a)}) {
        const double x = o[0] + d[0] * t, z = oz + d[2] * t;
        if (t <= 0 || z >= 0) continue;
        *u = r * std::atan2(x, -z), *v = o[1] + d[1] * t;
        return true;
    }
    return false;
}

// Where the mode leaves the controllers to a VR game (see the top): a hand controller
// pointing at a panel, its controls, or a floating window's popups keeps that panel's laser
// on (UpdateLasers) until kAimLinger ticks after it points away, like SteamVR's own floating
// windows. Leaving takes a wider margin than arriving, and a drag or a held button keeps it
// on. The keyboard is one overlay, so SteamVR's own intersection test does there.
void UpdateAim() {
    if (LasersByMode()) return;
    std::vector<Mat> lasers;
    for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
        Mat d;
        if (IsHandController(i) && LaserPose(i, &d)) lasers.push_back(d);
    }
    for (auto &[index, s] : g_screens) {
        Mat p;
        if (!s.visible || !ScreenPose(s, &p)) continue;
        if (s.drag != Drag::None || (g_press.buttons && g_press.screen == index)) {
            s.aimUntil = g_tick + kAimLinger;
            continue;
        }
        const double m = s.grip * (g_tick < s.aimUntil ? 2.0 : 0.25), h = s.heightMetres();
        // The panel and its controls: the bar row under it, the resize tab off its corner.
        const double halfW = std::max(s.metres / 2 + s.grip, s.chrome / 2 + s.chrome * 0.12 + s.grip * 2) + m;
        const double top = h / 2 + m, bottom = std::min(BarY(s) - s.grip, -(h / 2 + s.grip)) - m;
        for (const Mat &l : lasers) {
            double u, v;
            if (!RayOnSurface(s, p, l, &u, &v)) continue;
            bool on = std::fabs(u) <= halfW && v <= top && v >= bottom;
            for (const auto &[k, sub] : s.subs) {
                if (on || s.cropW <= 0) break;
                const double su = (sub.x + sub.w / 2.0 - (s.cropX + s.cropW / 2.0)) * s.mpp;
                const double sv = -(sub.y + sub.h / 2.0 - (s.cropY + s.cropH / 2.0)) * s.mpp;
                on = std::fabs(u - su) <= sub.w * s.mpp / 2 + m && std::fabs(v - sv) <= sub.h * s.mpp / 2 + m;
            }
            if (on) {
                s.aimUntil = g_tick + kAimLinger;
                break;
            }
        }
    }
    if (keyboard::Shown())
        for (const Mat &l : lasers)
            if (keyboard::Aimed(l)) g_keyboardAimUntil = g_tick + kAimLinger;
}

const char *LasersName() {
    switch (g_lasers) {
        case Lasers::Always: return "always";
        case Lasers::Dashboard: return "dashboard";
        default: return "outside_games";
    }
}

const char *ModeName() {
    switch (g_mode) {
        case Mode::Dashboard: return "dashboard";
        case Mode::Gesture: return "gesture";
        case Mode::Toggle: return "toggle";
        default: return "always";
    }
}

// ---------------------------------------------------------------- hand cutouts

void SetScreenTexture(const Screen &s, vr::SharedTextureHandle_t handle) {
    vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    vr::VROverlay()->SetOverlayTexture(s.overlay, &tex);
}

// The cutout buffers' renderer, set up the first time a hand is in front of a screen.
bool CutterReady() {
    if (g_cutterState) return g_cutterState > 0;
    uint64_t mods[64];
    const int n = ft_vr_modifiers(DRM_FORMAT_ABGR8888, mods, 64);
    const bool ok = g_cutter.Init(std::vector<uint64_t>(mods, mods + n), [](const handcut::Output *o) {
        auto it = g_cutImports.find(o);
        if (it == g_cutImports.end()) return;
        vr::VRIPCResourceManager()->UnrefResource(it->second);
        g_cutImports.erase(it);
    });
    g_cutterState = ok ? 1 : -1;
    std::printf(ok ? "hand cutouts ready\n" : "hand cutouts unavailable (see above)\n");
    return ok;
}

vr::SharedTextureHandle_t ImportCutout(const handcut::Output *o) {
    auto it = g_cutImports.find(o);
    if (it != g_cutImports.end()) return it->second;
    vr::DmabufAttributes_t a{};
    a.unWidth = uint32_t(o->buf.width);
    a.unHeight = uint32_t(o->buf.height);
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = o->buf.format;
    a.ulModifier = o->buf.modifier;
    a.unPlaneCount = uint32_t(o->buf.n_planes);
    for (int i = 0; i < o->buf.n_planes && i < int(vr::MaxDmabufPlaneCount); ++i) {
        a.plane[i].unOffset = o->buf.offset[i];
        a.plane[i].unStride = o->buf.stride[i];
        a.plane[i].nFd = o->buf.fd[i];
    }
    vr::SharedTextureHandle_t h = 0;
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &h)) {
        std::fprintf(stderr, "openvr: ImportDmabuf failed for a cutout buffer\n");
        h = 0;
    }
    g_cutImports.emplace(o, h);
    return h;
}

void StopCutting(Screen &s) {
    if (!s.cutting) return;
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SideBySide_Parallel, false);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    if (s.plain) SetScreenTexture(s, s.plain);
    s.cutting = false;
}

// Each tick: for each visible screen with a hand in front of it (for either eye), draw its
// client buffer with the hands cut out and show that; else show the client buffer.
// Floating windows don't get cutouts yet: their panel and popups show crops of the client
// buffer (texture bounds), which a side-by-side buffer doesn't match.
void UpdateCutouts() {
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    const bool hands = g_cutouts && haveHead &&
                       g_hands.Update(head, std::chrono::duration_cast<std::chrono::nanoseconds>(
                                                Clock::now().time_since_epoch()).count());
    double eyes[2][3];
    if (hands) handcut::EyePositions(head, eyes);
    for (auto &[i, s] : g_screens) {
        std::vector<handcut::Capsule2D> spots[2];
        Mat p;
        bool cut = hands && s.visible && !s.floating && s.key && s.width > 0 && ScreenPose(s, &p) &&
                   handcut::Project({p, s.metres, s.heightMetres(), s.curve, s.width, s.height}, g_hands.capsules(),
                                    eyes, spots);
        const handcut::Output *out = cut && CutterReady() ? g_cutter.Composite(i, s.key, s.buf, spots) : nullptr;
        const vr::SharedTextureHandle_t h = out ? ImportCutout(out) : 0;
        if (!h) {
            StopCutting(s);
            continue;
        }
        if (!s.cutting) {
            vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, false);
            vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SideBySide_Parallel, true);
            s.cutting = true;
        }
        SetScreenTexture(s, h);
    }
}

// Steam in front: the dashboard (the Steam menu) is open, or Steam's own keyboard is up
// (valve.steam.gamepadui.keyboard, for text fields in Steam and the dashboard). Our
// keyboard steps aside then, and comes back where it was when Steam is out of the way; one
// asked for meanwhile appears then. Checked every 9 ticks; Steam makes its keyboard's
// overlay again now and then, so it's looked up each time.
bool g_steamInFront = false;
bool g_keyboardAside = false;   // ours is waiting for Steam to get out of the way
Mat g_asidePose = Identity();   // ...and goes here then

bool SteamInFront() {
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    // In the dashboard mode the screens only show with the dashboard, so it doesn't count.
    return (g_mode != Mode::Dashboard && vr::VROverlay()->IsDashboardVisible()) ||
           (vr::VROverlay()->FindOverlay("valve.steam.gamepadui.keyboard", &h) == vr::VROverlayError_None &&
            vr::VROverlay()->IsOverlayVisible(h));
}

void UpdateSteamInFront() {
    const bool front = SteamInFront();
    if (front == g_steamInFront) return;
    g_steamInFront = front;
    if (front && keyboard::Shown()) {
        g_asidePose = keyboard::Pose();
        keyboard::Hide();
        g_keyboardAside = true;
    } else if (!front && g_keyboardAside) {
        g_keyboardAside = false;
        if (keyboard::Show(g_asidePose)) AnnounceOverlay("frametop.keyboard");
    }
}

}  // namespace

extern "C" {

bool ft_vr_init(void) {
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err == vr::VRInitError_None) {
        vr::VR_Shutdown();
        vr::VR_Init(&err, vr::VRApplication_Overlay);
    }
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return false;
    }
    if (!vr::VRIPCResourceManager()) {
        std::fprintf(stderr, "openvr: no IVRIPCResourceManagerClient (SteamVR too old?)\n");
        return false;
    }
    g_vr = true;
    RefreshPoses();
    // The catcher (see UpdateCatcher): clear and invisible, but the laser lands on it, and
    // it keeps SteamVR's laser mouse on while it's up.
    if (vr::VROverlay()->CreateOverlay("frametop.catcher", "Frametop: release catcher", &g_catcher) ==
        vr::VROverlayError_None) {
        static std::vector<uint8_t> clear(4 * 4 * 4, 0);
        vr::VROverlay()->SetOverlayRaw(g_catcher, clear.data(), 4, 4, 4);
        vr::VROverlay()->SetOverlayInputMethod(g_catcher, vr::VROverlayInputMethod_Mouse);
        vr::VROverlay()->SetOverlayAlpha(g_catcher, 0);
        vr::VROverlay()->SetOverlayFlag(g_catcher, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    }
    return true;
}

void ft_vr_shutdown(void) {
    if (!g_vr) return;
    if (g_cutterState == 1)
        for (auto &[i, s] : g_screens) g_cutter.DropPanel(i);  // drops their imports while SteamVR is up
    if (g_catcher != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(g_catcher);
    g_catcher = vr::k_ulOverlayHandleInvalid;
    for (auto &[i, s] : g_screens)
        for (auto o : s.All()) vr::VROverlay()->DestroyOverlay(o);
    for (auto &[dev, g] : g_guides)
        for (auto o : {g.ring.overlay, g.dot.overlay}) vr::VROverlay()->DestroyOverlay(o);
    for (auto &[k, h] : g_imports) vr::VRIPCResourceManager()->UnrefResource(h);
    keyboard::Destroy();
    g_guides.clear();
    g_screens.clear();
    g_imports.clear();
    vr::VR_Shutdown();
}

int ft_vr_modifiers(uint32_t format, uint64_t *out, int max) {
    if (!g_vr) {  // --no-vr: nothing imports the buffers, so any layout KWin can draw
        if (max < 1) return 0;
        out[0] = 0;  // DRM_FORMAT_MOD_LINEAR
        return 1;
    }
    uint32_t n = uint32_t(max);
    if (!vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, format, &n, out)) return 0;
    return int(n < uint32_t(max) ? n : uint32_t(max));
}

bool ft_vr_screens_shown(void) { return g_vr && ModeVisible(); }
bool ft_vr_paused(void) { return g_paused; }

bool ft_vr_screen_visible(int index) {
    const auto it = g_screens.find(index);
    return !g_vr || (it != g_screens.end() && it->second.visible);
}

enum ft_attention ft_vr_screen_attention(int index) {
    const auto it = g_screens.find(index);
    return g_vr && it != g_screens.end() ? it->second.attention : FT_FOCUSED;
}

bool ft_vr_vsync(double *since, double *hz) {
    if (!g_vr) return false;
    float s = 0;
    uint64_t frame = 0;
    if (!vr::VRSystem()->GetTimeSinceLastVsync(&s, &frame)) return false;
    vr::ETrackedPropertyError err = vr::TrackedProp_Success;
    const float f = vr::VRSystem()->GetFloatTrackedDeviceProperty(vr::k_unTrackedDeviceIndex_Hmd,
                                                                  vr::Prop_DisplayFrequency_Float, &err);
    if (err != vr::TrackedProp_Success || !(f >= 30 && f <= 240) || !(s >= 0 && s < 1)) return false;
    *since = s, *hz = f;
    return true;
}

}  // extern "C"

namespace {

// A panel and its controls. `prefix` names the overlays (frametop.screen.N,
// frametop.float.N), `label` is what SteamVR shows ("Screen 2", "Floating window 1").
bool MakePanel(Screen &s, const char *prefix, const char *label) {
    char key[64], name[64];
    std::snprintf(key, sizeof key, "%s", prefix);
    std::snprintf(name, sizeof name, "%s", label);
    if (vr::VROverlay()->CreateOverlay(key, name, &s.overlay) != vr::VROverlayError_None) {
        std::fprintf(stderr, "openvr: can't create overlay %s\n", key);
        return false;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(s.metres));
    vr::VROverlay()->SetOverlayInputMethod(s.overlay, vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    static const auto corner = CornerTexture(64);
    static const auto curve = CurveTexture(64);
    static const auto roll = RollTexture(64);
    auto chrome = [&](const char *part, const char *what, const std::vector<uint8_t> &px, int w, int h) {
        std::snprintf(key, sizeof key, "%s.%s", prefix, part);
        std::snprintf(name, sizeof name, "%s: %s", label, what);
        return MakeChrome(key, name, px, w, h);
    };
    s.bar = chrome("bar", "move", BarTexture(false), 256, 24);
    vr::VROverlay()->SetOverlayFlag(s.bar, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    s.curveButton = chrome("curve", "curve", curve, 64, 64);
    s.rollButton = chrome("roll", "roll", roll, 64, 64);
    vr::VROverlay()->SetOverlayFlag(s.rollButton, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    s.handle = chrome("resize", "resize", corner, 64, 64);
    if (s.floating) {
        static const auto dock = DockTexture(64);
        static const auto close = CloseTexture(64);
        s.dockButton = chrome("dock", "back to the desktop", dock, 64, 64);
        s.closeButton = chrome("close", "close", close, 64, 64);
    } else {
        static const auto reset = ResetTexture(64);
        s.resetButton = chrome("reset", "reset the layout", reset, 64, 64);
    }
    ApplyAlpha(s);
    return true;
}

}  // namespace

extern "C" {

void ft_vr_screen_create(int index, double metres, int count) {
    if (!g_vr) return;
    Screen &s = g_screens[index];
    s.metres = metres;
    char prefix[64], label[64];
    std::snprintf(prefix, sizeof prefix, "frametop.screen.%d", index + 1);
    std::snprintf(label, sizeof label, "Screen %d", index + 1);
    if (!MakePanel(s, prefix, label)) return;
    // Until the layout places it: 2 m ahead of the head, in a row, screen 1 on the left.
    RefreshPoses();
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
    const double heading = std::atan2(head.m[0][2], head.m[2][2]) * 180 / M_PI;
    const double yaw = heading + (double(count - 1) / 2 - index) * 35;
    const double dx = -std::sin(yaw * M_PI / 180), dz = -std::cos(yaw * M_PI / 180);
    SetAbsolute(s, PanelPose(head.m[0][3] + dx * 2, head.m[1][3], head.m[2][3] + dz * 2, yaw, 0, 0));
}

// A spare output's panel (number `slot` from 1): hidden until a window floats on it.
void ft_vr_float_create(int index, int slot) {
    if (!g_vr) return;
    Screen &s = g_screens[index];
    s.floating = true;
    char prefix[64], label[64];
    std::snprintf(prefix, sizeof prefix, "frametop.float.%d", slot);
    std::snprintf(label, sizeof label, "Floating window %d", slot);
    MakePanel(s, prefix, label);
}

void ft_vr_float_output(int index, bool on) {
    auto it = g_screens.find(index);
    if (it != g_screens.end()) it->second.outputOn = on;
}

void ft_vr_screen_destroy(int index) {
    auto it = g_screens.find(index);
    if (it == g_screens.end()) return;
    if (g_cutterState == 1) g_cutter.DropPanel(index);
    for (auto o : it->second.All())
        if (o != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(o);
    for (auto &[k, sub] : it->second.subs) vr::VROverlay()->DestroyOverlay(sub.overlay);
    g_screens.erase(it);
}

bool ft_vr_screen_present(int index, const void *key, const struct ft_dmabuf *b) {
    if (!g_vr) return false;
    auto sit = g_screens.find(index);
    if (sit == g_screens.end()) return false;
    Screen &s = sit->second;
    auto it = g_imports.find(key);
    if (it == g_imports.end()) {
        vr::DmabufAttributes_t a{};
        a.unWidth = uint32_t(b->width);
        a.unHeight = uint32_t(b->height);
        a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
        a.unFormat = b->format;
        a.ulModifier = b->modifier;
        a.unPlaneCount = uint32_t(b->n_planes);
        for (int i = 0; i < b->n_planes && i < int(vr::MaxDmabufPlaneCount); ++i) {
            a.plane[i].unOffset = b->offset[i];
            a.plane[i].unStride = b->stride[i];
            a.plane[i].nFd = b->fd[i];
        }
        vr::SharedTextureHandle_t h = 0;
        if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &h)) {
            std::fprintf(stderr, "openvr: ImportDmabuf failed: %dx%d format 0x%x modifier 0x%llx\n", b->width,
                         b->height, b->format, (unsigned long long)b->modifier);
            return false;
        }
        it = g_imports.emplace(key, h).first;
    }
    if (b->width != s.width || b->height != s.height) {
        s.width = b->width, s.height = b->height;
        if (s.floating) {
            ApplyCrop(s);
        } else {
            vr::HmdVector2_t scale = {float(s.width), float(s.height)};
            vr::VROverlay()->SetOverlayMouseScale(s.overlay, &scale);
        }
        PlaceChrome(s);  // the height changed
        std::printf("screen %d: %dx%d\n", index + 1, s.width, s.height);
    }
    s.key = key, s.buf = *b, s.plain = it->second;
    // While cutting, the next tick draws the new buffer with the cutouts (never floating).
    if (!s.cutting) SetScreenTexture(s, it->second);
    vr::SharedTextureHandle_t handle = it->second;
    vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    for (const auto &[k, sub] : s.subs) vr::VROverlay()->SetOverlayTexture(sub.overlay, &tex);
    s.shown = key;  // UpdateVisibility shows it on the next tick
    return true;
}

void ft_vr_forget(const void *key) {
    if (g_cutterState == 1) g_cutter.Forget(key);
    for (auto &[i, s] : g_screens)
        if (s.key == key) s.key = nullptr;
    auto it = g_imports.find(key);
    if (it == g_imports.end()) return;
    vr::VRIPCResourceManager()->UnrefResource(it->second);
    g_imports.erase(it);
}

void ft_vr_poll(void (*handle)(const struct ft_event *, void *), void *data) {
    if (!g_vr) return;
    RefreshPoses();
    for (auto &[index, s] : g_screens) {
        vr::VREvent_t ev;
        // The screen itself (and a floating window's popups): input for KWin.
        auto panelEvent = [&](const vr::VREvent_t &ev, bool sub) {
            ft_event e{};
            e.screen = index;
            if (ev.eventType != vr::VREvent_FocusLeave) s.inputMs = NowMs();
            auto at = [&] { s.ToBuffer(ev.data.mouse.x, ev.data.mouse.y, &e.x, &e.y); };
            switch (ev.eventType) {
                case vr::VREvent_MouseMove:
                    if (s.titleCarry) return;  // KWin's pointer stays where the title bar was pressed
                    e.type = FT_MOTION;
                    at();
                    if (g_press.buttons) g_press.screen = index, g_press.x = e.x, g_press.y = e.y;
                    break;
                case vr::VREvent_MouseButtonDown:
                case vr::VREvent_MouseButtonUp:
                    if (ev.eventType == vr::VREvent_MouseButtonUp) EndDragsBy(ev.trackedDeviceIndex);
                    e.type = FT_BUTTON;
                    e.button = LinuxButton(ev.data.mouse.button);
                    e.pressed = ev.eventType == vr::VREvent_MouseButtonDown;
                    e.controller = IsHandController(ev.trackedDeviceIndex);
                    at();
                    if (!e.pressed && s.titleCarry) e.x = s.carryX, e.y = s.carryY, s.titleCarry = false;
                    if (e.pressed) {
                        PressDown(ev.trackedDeviceIndex, e.button, index, e.x, e.y);
                        // A floating window's title bar: carry the panel, and KWin (which starts
                        // moving the window on the press) sees no motion until the release.
                        if (!sub && s.floating && e.button == BTN_LEFT && s.titleH > 0 && e.y >= s.cropY &&
                            e.y < s.cropY + s.titleH && s.drag == Drag::None) {
                            s.titleCarry = true, s.carryX = e.x, s.carryY = e.y;
                            StartDrag(s, Drag::Move, ev.trackedDeviceIndex);
                        }
                    } else {
                        g_press.buttons &= ~ButtonBit(e.button);
                        if (!g_press.buttons) g_press.upAt = -1;
                    }
                    break;
                case vr::VREvent_ScrollDiscrete:
                    e.type = FT_SCROLL;
                    e.dx = -ev.data.scroll.xdelta;
                    e.dy = -ev.data.scroll.ydelta;
                    break;
                case vr::VREvent_FocusLeave:
                    if (g_press.buttons) return;  // KWin keeps the pointer while a button is held
                    if (sub) return;              // off a popup is usually onto its window
                    e.type = FT_LEAVE;
                    break;
                default:
                    return;
            }
            handle(&e, data);
        };
        while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) panelEvent(ev, false);
        for (const auto &[k, sub] : s.subs)
            while (vr::VROverlay()->PollNextOverlayEvent(sub.overlay, &ev, sizeof ev)) panelEvent(ev, true);
        // The controls light up under a laser.
        auto hover = [&](int k) {
            const bool on = ev.eventType == vr::VREvent_MouseMove || ev.eventType == vr::VREvent_FocusEnter;
            if (on) s.inputMs = NowMs();
            if (!on && ev.eventType != vr::VREvent_FocusLeave) return;
            if (s.hover[k] != on) s.hover[k] = on, ApplyAlpha(s);
        };
        // The bar: move (and push/pull with the wheel while moving).
        while (vr::VROverlay()->PollNextOverlayEvent(s.bar, &ev, sizeof ev)) {
            hover(0);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Move, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
            else if (ev.eventType == vr::VREvent_ScrollDiscrete && s.drag == Drag::Move)
                Push(s, ev.data.scroll.ydelta);
        }
        // The corner: resize.
        while (vr::VROverlay()->PollNextOverlayEvent(s.handle, &ev, sizeof ev)) {
            hover(3);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Resize, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
        }
        // The curve button.
        while (vr::VROverlay()->PollNextOverlayEvent(s.curveButton, &ev, sizeof ev)) {
            hover(1);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                ToggleCurve(s);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
        }
        // A floating window's buttons: back to the desktop, and close (ft-floatd does both).
        for (int k : {4, 5}) {
            const vr::VROverlayHandle_t o = s.Controls()[k];
            if (o == vr::k_ulOverlayHandleInvalid) continue;
            while (vr::VROverlay()->PollNextOverlayEvent(o, &ev, sizeof ev)) {
                hover(k);
                if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left) {
                    SendFloat((k == 4 ? "dock " : "close ") + std::to_string(index + 1));
                } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                    EndDragsBy(ev.trackedDeviceIndex);
                    ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
                }
            }
        }
        // The reset button: every screen back in the layout, around where you are now
        // (`ft-layout apply`, like Meta+Shift+R; it refuses a second copy).
        while (s.resetButton != vr::k_ulOverlayHandleInvalid &&
               vr::VROverlay()->PollNextOverlayEvent(s.resetButton, &ev, sizeof ev)) {
            hover(6);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left) {
                std::printf("screen %d: reset the layout\n", index + 1);
                RunLayout("apply");
            } else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
        }
        // The roll button: drag around like a knob, or scroll.
        while (vr::VROverlay()->PollNextOverlayEvent(s.rollButton, &ev, sizeof ev)) {
            hover(2);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Roll, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp) {
                EndDragsBy(ev.trackedDeviceIndex);
                ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
            }
            else if (ev.eventType == vr::VREvent_ScrollDiscrete && s.drag == Drag::None) {
                Mat p;
                if (!ScreenPose(s, &p)) continue;
                s.rollFrom = s.pinned != kNone ? s.pinRel : p;
                ApplyRoll(s, ev.data.scroll.ydelta * kRollStep * M_PI / 180);
            }
        }
        if (s.drag != Drag::None) UpdateDrag(s, index);
    }
    // A release on the catcher, or the pointer helper's word that left came up (see "up").
    vr::VREvent_t ev;
    while (g_catcher != vr::k_ulOverlayHandleInvalid &&
           vr::VROverlay()->PollNextOverlayEvent(g_catcher, &ev, sizeof ev))
        if (ev.eventType == vr::VREvent_MouseButtonUp)
            ReleaseAwayBy(ev.trackedDeviceIndex, ev.data.mouse.button, handle, data);
    if (g_press.upAt >= 0 && g_tick >= g_press.upAt) ReleaseAway(BTN_LEFT, handle, data);
    ReleaseStuck(handle, data);
    RefreshChrome();
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) {
            ft_event e{};
            e.type = FT_QUIT;
            handle(&e, data);
        }
        // A carrying controller that goes away drops its screen.
        if (ev.eventType == vr::VREvent_TrackedDeviceDeactivated) EndDragsBy(ev.trackedDeviceIndex);
    }
    // Our keyboard: its keys, and its Close key. It goes when the screens do.
    struct Forward {
        void (*handle)(const struct ft_event *, void *);
        void *data;
    } forward{handle, data};
    keyboard::Poll(
        [](const keyboard::Event &k, void *f) {
            ft_event e{};
            e.screen = -1;
            e.type = k.type == keyboard::Event::Key ? FT_KEY : FT_KEYBOARD_CLOSED;
            e.key = k.code;
            e.pressed = k.pressed;
            static_cast<Forward *>(f)->handle(&e, static_cast<Forward *>(f)->data);
        },
        &forward);
    if (g_tick % 9 == 0) UpdateSteamInFront();
    if ((keyboard::Shown() || g_keyboardAside) && !ModeVisible()) {
        g_keyboardAside = false;
        keyboard::Hide();
        ft_event e{};
        e.type = FT_KEYBOARD_CLOSED;
        e.screen = -1;
        handle(&e, data);
    }
    ++g_tick;
    UpdateSpin();
    if (g_spin.front >= 0) {
        ft_event e{};
        e.type = FT_FRONT;
        e.screen = g_spin.front;
        g_spin.front = -1;
        handle(&e, data);
    }
    UpdateGame();
    UpdateArrange();
    UpdateVisibility();
    UpdateAttention();
    UpdateAim();
    UpdateLasers();
    UpdateControls();
    UpdateGuides();
    UpdateCutouts();
    UpdateCatcher();
}

// Our keyboard (keyboard.cpp) for a screen. It's placed where you'll reach it, not on the
// screen: kKeyboardAhead in front of you (the way your head faces, level) and
// kKeyboardBelow under your eyes, turned to face your eyes, and it stays where it opened
// (or where its grab bar carries it). With Steam in front (UpdateSteamInFront), it waits.
// Without a head pose (the headset is in standby, say) it doesn't open: anywhere else could
// be out of sight or reach. The next text field opens it.
constexpr double kKeyboardAhead = 0.7, kKeyboardBelow = 0.35;

bool ft_vr_keyboard_show(int index) {
    if (g_screens.find(index) == g_screens.end()) return false;
    RefreshPoses();
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) {
        std::printf("keyboard: no head pose, not opened\n");
        return false;
    }
    const double fx = -head.m[0][2], fz = -head.m[2][2], n = std::sqrt(fx * fx + fz * fz) + 1e-9;
    const double at[3] = {head.m[0][3] + fx / n * kKeyboardAhead, head.m[1][3] - kKeyboardBelow,
                          head.m[2][3] + fz / n * kKeyboardAhead};
    keyboard::SetLasers(LasersByMode());
    g_steamInFront = SteamInFront();
    if (g_steamInFront) {
        g_asidePose = FacingPose(at, head);
        g_keyboardAside = true;
        std::printf("keyboard: waiting for Steam to close\n");
        return true;
    }
    if (!keyboard::Show(FacingPose(at, head))) return false;
    AnnounceOverlay("frametop.keyboard");
    return true;
}

void ft_vr_keyboard_hide(void) {
    g_keyboardAside = false;
    keyboard::Hide();
}

// Control commands (datagrams on @ft_screens, replies to the sender):
//   place <screen> <x> <y> <z> <yaw> <pitch> <roll>   centre (standing universe) and facing
//   width <screen> <metres>
//   curve <screen> <radius>   cylinder radius in metres; 0 = flat
//   curve <screen> on|off     on: the radius is the head's distance to it now -> "ok <radius>"
//   pin <screen|all> <left|right|head> [12 numbers]   pin to that hand's controller or the
//                             headset: as it is now, or at the given device->screen transform
//                             (rows of a 3x4)
//   unpin <screen|all>
//   get <screen>  -> "ok x y z  xx xy xz  yx yy yz  zx zy zz  width height curve pin
//                     [12 numbers: device->screen, when pinned]"   (pin: none|left|right|head)
//   screens       -> "ok <count> <index>:<pixels w>x<h>:<metres> ..."
//   head          -> "ok x y z yaw"
//   visibility always|dashboard|gesture|toggle
//   wrist <degrees>           a pinned screen shows while you see its front within this
//   gesture <left|right> <degrees>   the gesture mode: look within this of that controller
//   hide | show | toggle      the manual switch (see g_manual)
//   conceal <screen|all> | reveal <screen|all>   a screen hidden on its own, whatever the mode
//   concealed     -> "ok [<screen> ...]"   the screens hidden on their own
//   controllers always|outside_games|dashboard   when controllers' lasers work the screens
//   ingames hide|visible      during a VR game, "always" acts like "only with the dashboard"
//                             (hide), or stays as it is (visible)
//   up                        the pointer helper: the mouse's left button came up. If SteamVR
//                             hasn't delivered that release to one of our overlays within
//                             ~100 ms (it landed on something else), KWin gets it anyway
//   state         -> "ok <mode> <manual 0|1> <wrist deg> <gesture hand> <gesture deg>
//                     <controllers> <game running 0|1> <ingames>"
//   spin next|prev|<degrees>  turn every panel in the room about your head (see the lazy
//                             susan) -> "ok <degrees turned>"
//   cutouts on|off|state      hand cutouts (see handcut.h) -> "ok <on|off> <ready|idle|unavailable>
//                             <last composite ms> ms, predict <on|off> lead <ms> ms"
//   cutouts predict on|off    move the hands ahead along their velocity (on by default)
//   cutouts lead <ms>         ...to this long after now: about when the frame is on the displays
// Floating windows (from ft-floatd; <screen> is the spare output's number, after the screens):
//   float <screen> <metres per pixel> <x> <y> <w> <h> <title>   the window's rectangle in the
//                             buffer and its title bar's height (pixels); shows the panel
//   unfloat <screen>          hides it
//   pose <screen> <12 numbers>  its place in the room (rows of a 3x4, standing universe)
//   sub <screen> <k> <x> <y> <w> <h> | sub <screen> <k> off   popup or dialog k over it
//   minimized <screen> 0|1
//   carry <screen>            the window's own title bar was pressed (an app that draws its
//                             own): carry the panel with the pressing laser until the release
// (size <screen> <w> <h> and key <code> <value> are handled in compositor.c.) Screens are
// numbered from 1 here, like everywhere the user sees them. "screens" and "all" leave out
// floating windows.
void ft_vr_command(const char *cmd, char *reply, int size) {
    if (!g_vr) return (void)std::snprintf(reply, size, "error no SteamVR (--no-vr)");
    RefreshPoses();
    int n;
    double x, y, z, yaw, pitch, roll, w;
    char word[16], hand[16];
    float r[12];
    auto each = [&](const char *which, auto fn) -> bool {  // "all" or a screen number
        if (std::strcmp(which, "all") == 0) {
            for (auto &[i, s] : g_screens)
                if (!s.floating) fn(s);
            return true;
        }
        Screen *s = Find(std::atoi(which));
        if (s) fn(*s);
        return s != nullptr;
    };
    if (std::sscanf(cmd, "place %d %lf %lf %lf %lf %lf %lf", &n, &x, &y, &z, &yaw, &pitch, &roll) == 7) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        EndDrag(*s);
        g_spin.base.erase(n - 1);
        SetAbsolute(*s, PanelPose(x, y, z, yaw, pitch, roll));
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "width %d %lf", &n, &w) == 2) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        SetWidth(*s, w);
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "curve %d %7s", &n, word) == 2 && (!std::strcmp(word, "on") || !std::strcmp(word, "off"))) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        if ((s->curve > 0) != (word[1] == 'n')) ToggleCurve(*s);
        std::snprintf(reply, size, "ok %.3f", s->curve);
    } else if (std::sscanf(cmd, "curve %d %lf", &n, &w) == 2) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        s->curve = w > 0 ? std::max(0.5, w) : 0;
        ApplyCurve(*s);
        PlaceChrome(*s);
        std::snprintf(reply, size, "ok");
    } else if (const int got = std::sscanf(cmd, "pin %15s %15s %f %f %f %f %f %f %f %f %f %f %f %f", word, hand,
                                           &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7], &r[8], &r[9],
                                           &r[10], &r[11]);
               got >= 2) {
        if (std::strcmp(hand, "left") && std::strcmp(hand, "right") && std::strcmp(hand, "head"))
            return (void)std::snprintf(reply, size, "error pin to left, right, or head");
        const vr::TrackedDeviceIndex_t dev = HandDevice(hand);
        Mat c;
        if (dev == vr::k_unTrackedDeviceIndex_Hmd && !DevicePose(dev, &c))
            return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
        if (dev == kNone || !DevicePose(dev, &c))
            return (void)std::snprintf(reply, size, "error no %s controller tracked", hand);
        Mat rel = Identity();
        for (int k = 0; k < 12; ++k) rel.m[k / 4][k % 4] = r[k];
        const bool found = each(word, [&](Screen &s) {
            Mat p;
            EndDrag(s);
            if (got == 14) Pin(s, dev, rel);
            else if (ScreenPose(s, &p)) Pin(s, dev, Mul(Inverse(c), p));
        });
        std::snprintf(reply, size, found ? "ok" : "error no such screen");
    } else if (std::sscanf(cmd, "unpin %15s", word) == 1) {
        const bool found = each(word, [&](Screen &s) {
            Mat p;
            if (s.pinned != kNone && ScreenPose(s, &p)) SetAbsolute(s, p);
        });
        std::snprintf(reply, size, found ? "ok" : "error no such screen");
    } else if (std::sscanf(cmd, "get %d", &n) == 1) {
        Screen *s = Find(n);
        Mat m;
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        if (!ScreenPose(*s, &m)) return (void)std::snprintf(reply, size, "error screen %d has no pose", n);
        int len = std::snprintf(reply, size,
                                "ok %.4f %.4f %.4f  %.5f %.5f %.5f  %.5f %.5f %.5f  %.5f %.5f %.5f  %.4f %.4f %.3f %s",
                                m.m[0][3], m.m[1][3], m.m[2][3], m.m[0][0], m.m[1][0], m.m[2][0], m.m[0][1], m.m[1][1],
                                m.m[2][1], m.m[0][2], m.m[1][2], m.m[2][2], s->metres, s->heightMetres(), s->curve,
                                s->pinned == kNone ? "none" : HandName(s->pinned));
        if (s->pinned != kNone)
            for (int k = 0; k < 12 && len < size; ++k)
                len += std::snprintf(reply + len, size - len, " %.5f", s->pinRel.m[k / 4][k % 4]);
    } else if (std::strncmp(cmd, "screens", 7) == 0) {
        const size_t count = std::count_if(g_screens.begin(), g_screens.end(), [](auto &e) { return !e.second.floating; });
        int len = std::snprintf(reply, size, "ok %zu", count);
        for (auto &[i, s] : g_screens)
            if (len < size && !s.floating)
                len += std::snprintf(reply + len, size - len, " %d:%dx%d:%.3f", i + 1, s.width, s.height, s.metres);
    } else if (std::strncmp(cmd, "head", 4) == 0) {
        Mat m;
        if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &m))
            return (void)std::snprintf(reply, size, "error no head pose (headset off?)");
        std::snprintf(reply, size, "ok %.4f %.4f %.4f %.2f", m.m[0][3], m.m[1][3], m.m[2][3],
                      std::atan2(m.m[0][2], m.m[2][2]) * 180 / M_PI);
    } else if (std::sscanf(cmd, "visibility %15s", word) == 1) {
        const std::string m = word;
        if (m == "always") g_mode = Mode::Always;
        else if (m == "dashboard") g_mode = Mode::Dashboard;
        else if (m == "gesture") g_mode = Mode::Gesture;
        else if (m == "toggle") g_mode = Mode::Toggle;
        else return (void)std::snprintf(reply, size, "error modes: always dashboard gesture toggle");
        g_manual = false;
        std::snprintf(reply, size, "ok %s", ModeName());
    } else if (std::sscanf(cmd, "wrist %lf", &w) == 1) {
        g_wristAngle = std::clamp(w, 10.0, 180.0);
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "gesture %15s %lf", hand, &w) == 2) {
        g_gestureHand = std::strcmp(hand, "right") == 0 ? "right" : "left";
        g_gestureAngle = std::clamp(w, 5.0, 90.0);
        std::snprintf(reply, size, "ok");
    } else if (std::strncmp(cmd, "concealed", 9) == 0) {
        int len = std::snprintf(reply, size, "ok");
        for (auto &[i, s] : g_screens)
            if (len < size && !s.floating && s.alone) len += std::snprintf(reply + len, size - len, " %d", i + 1);
    } else if (std::sscanf(cmd, "conceal %15s", word) == 1 || std::sscanf(cmd, "reveal %15s", word) == 1) {
        const bool hide = cmd[0] == 'c';
        const Screen *one = std::strcmp(word, "all") ? Find(std::atoi(word)) : nullptr;
        if (std::strcmp(word, "all") && (!one || one->floating))
            return (void)std::snprintf(reply, size, "error no screen %s", word);
        each(word, [&](Screen &s) { s.alone = hide; });
        UpdateVisibility();
        std::snprintf(reply, size, "ok");
    } else if (!std::strncmp(cmd, "hide", 4) || !std::strncmp(cmd, "show", 4) || !std::strncmp(cmd, "toggle", 6)) {
        const bool always = EffectiveMode() == Mode::Always;
        const bool shownNow = always ? !g_manual : g_manual;
        const bool want = cmd[0] == 's' ? true : cmd[0] == 'h' ? false : !shownNow;
        g_manual = always ? !want : want;
        UpdateVisibility();
        std::snprintf(reply, size, "ok %s", want ? "shown" : "hidden");
    } else if (std::sscanf(cmd, "pause %15s", word) == 1) {
        if (!std::strcmp(word, "on") || !std::strcmp(word, "off")) {
            const bool on = !std::strcmp(word, "on");
            if (on != g_paused)
                std::printf("%s\n", on ? "paused for a VR game: everything hidden, KWin slowed down" : "resumed");
            g_paused = on;
            UpdateVisibility();
        } else if (std::strcmp(word, "state") != 0) {
            return (void)std::snprintf(reply, size, "error pause on|off|state");
        }
        std::snprintf(reply, size, "ok %s", g_paused ? "paused" : "running");
    } else if (std::sscanf(cmd, "ingames %15s", word) == 1) {
        if (!std::strcmp(word, "hide")) g_inGames = InGames::Hide;
        else if (!std::strcmp(word, "visible")) g_inGames = InGames::Visible;
        else return (void)std::snprintf(reply, size, "error modes: hide visible");
        g_manual = false;
        UpdateVisibility();
        std::snprintf(reply, size, "ok %s", word);
    } else if (std::sscanf(cmd, "controllers %15s", word) == 1) {
        const std::string m = word;
        if (m == "always") g_lasers = Lasers::Always;
        else if (m == "outside_games") g_lasers = Lasers::OutsideGames;
        else if (m == "dashboard") g_lasers = Lasers::Dashboard;
        else return (void)std::snprintf(reply, size, "error modes: always outside_games dashboard");
        UpdateLasers();
        std::snprintf(reply, size, "ok %s", LasersName());
    } else if (std::sscanf(cmd, "cutouts %15s", word) == 1) {
        char arg[16] = "";
        double ms = 0;
        if (!std::strcmp(word, "on")) g_cutouts = true;
        else if (!std::strcmp(word, "off")) g_cutouts = false;
        else if (!std::strcmp(word, "predict") && std::sscanf(cmd, "cutouts predict %15s", arg) == 1 &&
                 (!std::strcmp(arg, "on") || !std::strcmp(arg, "off")))
            g_hands.SetPrediction(!std::strcmp(arg, "on"), g_hands.leadMs());
        else if (!std::strcmp(word, "lead") && std::sscanf(cmd, "cutouts lead %lf", &ms) == 1)
            g_hands.SetPrediction(g_hands.predicting(), ms);
        else if (std::strcmp(word, "state") != 0)
            return (void)std::snprintf(reply, size, "error cutouts on|off|state|predict on|off|lead <ms>");
        std::snprintf(reply, size, "ok %s %s %.2f ms, predict %s lead %.0f ms", g_cutouts ? "on" : "off",
                      g_cutterState > 0 ? "ready" : g_cutterState < 0 ? "unavailable" : "idle", g_cutter.lastMs(),
                      g_hands.predicting() ? "on" : "off", g_hands.leadMs());
    } else if (int x0, y0, w0, h0, t0; std::sscanf(cmd, "float %d %lf %d %d %d %d %d", &n, &w, &x0, &y0, &w0, &h0, &t0) == 7) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        if (!(w > 1e-5 && w < 0.01) || w0 < 1 || h0 < 1) return (void)std::snprintf(reply, size, "error bad float");
        SetFloat(*s, w, x0, y0, w0, h0, std::max(0, t0));
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "unfloat %d", &n) == 1) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        Unfloat(*s);
        UpdateVisibility();
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "pose %d %f %f %f %f %f %f %f %f %f %f %f %f", &n, &r[0], &r[1], &r[2], &r[3], &r[4],
                           &r[5], &r[6], &r[7], &r[8], &r[9], &r[10], &r[11]) == 13) {
        Screen *s = Find(n);
        if (!s) return (void)std::snprintf(reply, size, "error no screen %d", n);
        Mat m{};
        for (int k = 0; k < 12; ++k) m.m[k / 4][k % 4] = r[k];
        EndDrag(*s);
        g_spin.base.erase(n - 1);
        SetAbsolute(*s, m);
        std::snprintf(reply, size, "ok");
    } else if (int k0; std::sscanf(cmd, "sub %d %d %d %d %d %d", &n, &k0, &x0, &y0, &w0, &h0) == 6 ||
                       (std::sscanf(cmd, "sub %d %d %15s", &n, &k0, word) == 3 && !std::strcmp(word, "off"))) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        if (std::strstr(cmd, " off")) w0 = h0 = 0;
        SetSub(*s, n - 1, k0, x0, y0, w0, h0);
        std::snprintf(reply, size, "ok");
    } else if (int on; std::sscanf(cmd, "minimized %d %d", &n, &on) == 2) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        s->minimized = on != 0;
        UpdateVisibility();
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "carry %d", &n) == 1) {
        Screen *s = Find(n);
        if (!s || !s->floating) return (void)std::snprintf(reply, size, "error no floating window panel %d", n);
        if (!(g_press.buttons & ButtonBit(BTN_LEFT)) || g_press.screen != n - 1 || s->drag != Drag::None)
            return (void)std::snprintf(reply, size, "error not pressed there");
        s->titleCarry = true, s->carryX = g_press.x, s->carryY = g_press.y;
        StartDrag(*s, Drag::Move, g_press.device);
        std::snprintf(reply, size, "ok");
    } else if (std::strcmp(cmd, "up") == 0) {
        if ((g_press.buttons & ButtonBit(BTN_LEFT)) && g_press.device != kNone && !IsHandController(g_press.device))
            g_press.upAt = g_tick + 9;
        std::snprintf(reply, size, "ok");
    } else if (std::sscanf(cmd, "spin %15s", word) == 1) {
        SpinCommand(word, reply, size);
    } else if (std::strncmp(cmd, "state", 5) == 0) {
        std::snprintf(reply, size, "ok %s %d %.0f %s %.0f %s %d %s", ModeName(), g_manual ? 1 : 0, g_wristAngle,
                      g_gestureHand.c_str(), g_gestureAngle, LasersName(), g_gameRunning ? 1 : 0,
                      g_inGames == InGames::Hide ? "hide" : "visible");
    } else {
        std::snprintf(reply, size, "error unknown command");
    }
}

}  // extern "C"
