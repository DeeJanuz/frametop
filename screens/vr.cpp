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
//   - visibility modes: always (the hide hotkey toggles), only with the SteamVR dashboard
//     open, while you look at a chosen controller (the wrist gesture), or toggle only
//     (hidden until the hotkey shows them).
//   - controllers on the screens: while visible, the screens can keep SteamVR's laser mouse
//     on (VROverlayFlags_MakeOverlaysInteractiveIfVisible), so controllers use them with
//     the dashboard closed. That also takes the controllers away from a VR game, so by
//     default it's off while a game (a scene app) runs: the screens stay up over the game,
//     the controllers stay in it, and the 3D mouse (its own laser mode) or the dashboard
//     works the screens. Modes: always, outside_games (default), dashboard (never on its
//     own; also for flatscreen games, which aren't scene apps).
//   - during a VR game the screens hide unless the dashboard is open (g_inGames, default),
//     or stay visible over it; the hotkey still shows them.
// OpenVR has no overlay-relative transforms here (openvr v2.15.6), so the bar, button,
// and handle are placed whenever their screen moves.
#include "vr.h"

#include <openvr.h>

#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <limits.h>
#include <spawn.h>

extern char **environ;  // for posix_spawn

#include <algorithm>
#include <chrono>
#include <array>
#include <cmath>
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
    if (model[0] && vr::VRRenderModels()->GetComponentStateForDevicePath(model, vr::k_pch_Controller_Component_Tip,
                                                                          vr::k_ulInvalidInputValueHandle, &mode, &state))
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
vr::TrackedDeviceIndex_t HandDevice(const char *hand) {
    return vr::VRSystem()->GetTrackedDeviceIndexForControllerRole(
        std::strcmp(hand, "right") == 0 ? vr::TrackedControllerRole_RightHand : vr::TrackedControllerRole_LeftHand);
}
const char *HandName(vr::TrackedDeviceIndex_t i) {
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
long g_tick = 0;                     // ft_vr_poll calls
constexpr vr::TrackedDeviceIndex_t kNone = vr::k_unTrackedDeviceIndexInvalid;

struct Screen {
    vr::VROverlayHandle_t overlay = vr::k_ulOverlayHandleInvalid, bar = vr::k_ulOverlayHandleInvalid,
                          handle = vr::k_ulOverlayHandleInvalid, curveButton = vr::k_ulOverlayHandleInvalid,
                          rollButton = vr::k_ulOverlayHandleInvalid;
    int width = 0, height = 0;    // current buffer size (mouse scale)
    double metres = 1;
    double curve = 0;             // cylinder radius in metres; 0 = flat
    const void *shown = nullptr;  // a frame arrived
    bool visible = false;         // shown in VR right now
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
    bool hover[4] = {};                       // a laser is on the bar, curve, roll, resize control
    bool lasers = true;                       // MakeOverlaysInteractiveIfVisible is set
    float controls = 0;                       // the controls' fade, 0 (hidden) .. 1
    bool controlsUp = false;                  // the controls' overlays are shown
    long nearUntil = 0;                       // a laser was near the controls until this tick
    vr::TrackedDeviceIndex_t pinTarget = kNone;  // moving: rides on this controller when let go
    vr::TrackedDeviceIndex_t onWrist = kNone;    // moving: the laser is in this controller's ring
    bool barLit = false;
    double chrome = 0.3;          // the bar's width; the other controls follow it (ChromeSize)
    double grip = 0.04;           // the corner tab's and the round buttons' size
    double heightMetres() const { return width > 0 ? metres * height / width : metres * 9 / 16; }
    std::array<vr::VROverlayHandle_t, 4> Controls() const { return {bar, curveButton, rollButton, handle}; }
    std::array<vr::VROverlayHandle_t, 5> All() const { return {overlay, bar, curveButton, rollButton, handle}; }
};
std::map<int, Screen> g_screens;
std::map<const void *, vr::SharedTextureHandle_t> g_imports;

// Visibility (see the top). g_manual is the hide/show switch: in the always mode it hides
// the screens, in the others it shows them anyway.
Mode g_mode = Mode::Always;
bool g_manual = false;
double g_wristAngle = 60;    // a pinned screen shows while you see its front within this
double g_gestureAngle = 20;  // gesture: look within this of the controller
std::string g_gestureHand = "left";
Lasers g_lasers = Lasers::OutsideGames;  // when controllers' lasers work the screens (see the top)
bool g_gameRunning = false;              // a scene app (VR game) is running
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
// Where each control sits, relative to the screen: bar, curve, roll, resize tab.
std::array<Mat, 4> ControlOffsets(const Screen &s) {
    const double h = s.heightMetres(), bar = s.chrome, button = s.grip, gap = bar * 0.06;
    return {BarOffset(s), OnSurface(s, bar / 2 + gap + button / 2, BarY(s), 0.003),
            OnSurface(s, bar / 2 + gap * 2 + button * 1.5, BarY(s), 0.003),
            // The tab's top left corner is the screen's bottom right corner.
            OnSurface(s, s.metres / 2 + s.grip / 2, -(h / 2 + s.grip / 2), 0.003)};
}

void PlaceChrome(Screen &s) {
    ChromeSize(s);
    const double bar = s.chrome, button = s.grip;
    const auto offsets = ControlOffsets(s);
    vr::VROverlay()->SetOverlayWidthInMeters(s.bar, float(bar));
    vr::VROverlay()->SetOverlayWidthInMeters(s.curveButton, float(button));
    vr::VROverlay()->SetOverlayWidthInMeters(s.rollButton, float(button));
    vr::VROverlay()->SetOverlayWidthInMeters(s.handle, float(s.grip));
    // Curved, the bar bends with the screen's bottom edge.
    vr::VROverlay()->SetOverlayCurvature(s.bar, s.curve > 0 ? float(std::min(1.0, bar / (2 * M_PI * s.curve))) : 0.f);
    const std::pair<vr::VROverlayHandle_t, Mat> parts[] = {
        {s.bar, offsets[0]}, {s.curveButton, offsets[1]}, {s.rollButton, offsets[2]}, {s.handle, offsets[3]}};
    if (s.pinned != kNone) {
        for (const auto &[o, off] : parts) {
            const Mat m = Mul(s.pinRel, off);
            vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(o, s.pinned, &m);
        }
        return;
    }
    Mat p;
    if (!ScreenPose(s, &p)) return;
    for (const auto &[o, off] : parts) {
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
    const bool active[4] = {s.hover[0] || s.drag == Drag::Move, s.hover[1], s.hover[2] || s.drag == Drag::Roll,
                            s.hover[3] || s.drag == Drag::Resize};
    const auto controls = s.Controls();
    for (int k = 0; k < 4; ++k)
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
        return;
    }
    // Hidden: the controls go at once (UpdateControls brings them back).
    vr::VROverlay()->HideOverlay(s.overlay);
    for (auto o : s.Controls()) vr::VROverlay()->HideOverlay(o);
    s.controls = 0, s.controlsUp = false;
}

void UpdateVisibility() {
    const bool shared = ModeVisible();
    Mat head;
    const bool haveHead = DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head);
    for (auto &[i, s] : g_screens) {
        bool visible = s.shown && (shared || s.drag != Drag::None);
        float alpha = 1;
        Mat p;
        if (visible && s.pinned != kNone && s.drag == Drag::None && haveHead && ScreenPose(s, &p)) {
            // A pinned screen shows while you see its front: fully inside the wrist angle,
            // fading out over the last kFade degrees, gone beyond it (and from behind).
            const double a = FacingAngle(p, head);
            alpha = float(std::clamp((g_wristAngle - a) / kFade, 0.0, 1.0));
            visible = alpha > 0.02f;
        }
        SetVisible(s, visible, alpha);
    }
}


// Controllers' lasers on the screens (see the top): the flag follows the mode and whether a
// VR game runs.
void UpdateLasers() {
    const bool want = g_lasers == Lasers::Always || (g_lasers == Lasers::OutsideGames && !g_gameRunning);
    for (auto &[i, s] : g_screens) {
        if (s.lasers == want) continue;
        s.lasers = want;
        vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, want);
    }
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
            for (int k = 1; k < 4; ++k) spots.push_back(Mul(p, offsets[k]));
            const double reach = std::max(s.grip * 1.5, s.chrome * 0.12);
            for (const Mat &d : lasers) {
                const double o[3] = {d.m[0][3], d.m[1][3], d.m[2][3]}, dir[3] = {-d.m[0][2], -d.m[1][2], -d.m[2][2]};
                bool close = false;
                for (const Mat &c : spots) {
                    const double v[3] = {c.m[0][3] - o[0], c.m[1][3] - o[1], c.m[2][3] - o[2]};
                    const double t = Dot3(v, dir);
                    if (t <= 0) continue;
                    const double q[3] = {v[0] - dir[0] * t, v[1] - dir[1] * t, v[2] - dir[2] * t};
                    if (Dot3(q, q) <= reach * reach) close = true;
                }
                if (close) {
                    s.nearUntil = g_tick + kControlsLinger;
                    break;
                }
            }
        }
        const bool inUse = s.drag != Drag::None || s.hover[0] || s.hover[1] || s.hover[2] || s.hover[3];
        const bool want = s.visible && (inUse || g_tick < s.nearUntil);
        // The controls stay shown while their screen is, just fully transparent when not
        // wanted: SteamVR's laser still hits them, and the hover event brings them in, for
        // any device's laser, whatever its shape.
        if (s.visible && !s.controlsUp) {
            for (auto o : s.Controls()) vr::VROverlay()->ShowOverlay(o);
            s.controlsUp = true;
            ApplyAlpha(s);
        }
        const float before = s.controls;
        s.controls = std::clamp(s.controls + (want ? 0.2f : -0.1f), 0.f, 1.f);
        if (s.controls != before) ApplyAlpha(s);
    }
}

// ---------------------------------------------------------------- moving, resizing, pinning

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

// KWin's outputs follow where the screens are, so the pointer and dragged windows cross
// to the screen you see next to this one: `ft-layout scale` runs once a move has settled.
long g_arrangeAt = -1;  // g_tick to run it at, -1 = not pending

void ArrangeDesktopSoon() { g_arrangeAt = g_tick + 45; }  // about half a second

void UpdateArrange() {
    if (g_arrangeAt < 0 || g_tick < g_arrangeAt) return;
    g_arrangeAt = -1;
    char exe[PATH_MAX];
    if (!realpath("/proc/self/exe", exe)) return;
    std::string layout(exe);  // <repo>/screens/build/ft-screens -> <repo>/layout/ft-layout
    for (int up = 0; up < 3 && layout.rfind('/') != std::string::npos; ++up) layout.resize(layout.rfind('/'));
    layout += "/layout/ft-layout";
    posix_spawn_file_actions_t io;
    posix_spawn_file_actions_init(&io);
    posix_spawn_file_actions_addopen(&io, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&io, 1, "/tmp/frametop-layout.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
    posix_spawn_file_actions_adddup2(&io, 1, 2);
    char scale[] = "scale";
    char *argv[] = {layout.data(), scale, nullptr};
    pid_t pid;  // reaped by the compositor's SIGCHLD handler
    if (posix_spawn(&pid, layout.c_str(), &io, nullptr, argv, environ) != 0)
        std::printf("can't run %s\n", layout.c_str());
    posix_spawn_file_actions_destroy(&io);
}

// Let go: pin to the armed wrist, as the screen is now.
void FinishDrag(Screen &s, int index) {
    const bool moved = s.drag == Drag::Move;
    const vr::TrackedDeviceIndex_t target = s.pinTarget;
    EndDrag(s);
    Mat c, p;
    if (!moved) return;
    ArrangeDesktopSoon();
    if (target != kNone && DevicePose(target, &c) && ScreenPose(s, &p)) {
        Pin(s, target, Mul(Inverse(c), p));
        std::printf("screen %d: pinned to the %s controller\n", index + 1, HandName(target));
    }
}

// A button release on any of our panels ends that device's drags (it may be over another
// screen by then).
void EndDragsBy(vr::TrackedDeviceIndex_t dev) {
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

Screen *Find(int one_based) {
    auto it = g_screens.find(one_based - 1);
    return it == g_screens.end() ? nullptr : &it->second;
}

uint32_t LinuxButton(uint32_t vrButton) {
    switch (vrButton) {
        case vr::VRMouseButton_Right: return BTN_RIGHT;
        case vr::VRMouseButton_Middle: return BTN_MIDDLE;
        default: return BTN_LEFT;
    }
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
    RefreshPoses();
    return true;
}

void ft_vr_shutdown(void) {
    for (auto &[i, s] : g_screens)
        for (auto o : s.All()) vr::VROverlay()->DestroyOverlay(o);
    for (auto &[dev, g] : g_guides)
        for (auto o : {g.ring.overlay, g.dot.overlay}) vr::VROverlay()->DestroyOverlay(o);
    for (auto &[k, h] : g_imports) vr::VRIPCResourceManager()->UnrefResource(h);
    g_guides.clear();
    g_screens.clear();
    g_imports.clear();
    vr::VR_Shutdown();
}

int ft_vr_modifiers(uint32_t format, uint64_t *out, int max) {
    uint32_t n = uint32_t(max);
    if (!vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, format, &n, out)) return 0;
    return int(n < uint32_t(max) ? n : uint32_t(max));
}

bool ft_vr_screens_shown(void) { return ModeVisible(); }

void ft_vr_screen_create(int index, double metres, int count) {
    Screen &s = g_screens[index];
    s.metres = metres;
    char key[64], name[64];
    std::snprintf(key, sizeof key, "frametop.screen.%d", index + 1);
    std::snprintf(name, sizeof name, "Screen %d", index + 1);
    if (vr::VROverlay()->CreateOverlay(key, name, &s.overlay) != vr::VROverlayError_None) {
        std::fprintf(stderr, "openvr: can't create overlay %s\n", key);
        return;
    }
    vr::VROverlay()->SetOverlayWidthInMeters(s.overlay, float(metres));
    vr::VROverlay()->SetOverlayInputMethod(s.overlay, vr::VROverlayInputMethod_Mouse);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    vr::VROverlay()->SetOverlayFlag(s.overlay, vr::VROverlayFlags_MakeOverlaysInteractiveIfVisible, true);
    static const auto corner = CornerTexture(64);
    static const auto curve = CurveTexture(64);
    static const auto roll = RollTexture(64);
    std::snprintf(key, sizeof key, "frametop.screen.%d.bar", index + 1);
    std::snprintf(name, sizeof name, "Screen %d: move", index + 1);
    s.bar = MakeChrome(key, name, BarTexture(false), 256, 24);
    vr::VROverlay()->SetOverlayFlag(s.bar, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    std::snprintf(key, sizeof key, "frametop.screen.%d.curve", index + 1);
    std::snprintf(name, sizeof name, "Screen %d: curve", index + 1);
    s.curveButton = MakeChrome(key, name, curve, 64, 64);
    std::snprintf(key, sizeof key, "frametop.screen.%d.roll", index + 1);
    std::snprintf(name, sizeof name, "Screen %d: roll", index + 1);
    s.rollButton = MakeChrome(key, name, roll, 64, 64);
    vr::VROverlay()->SetOverlayFlag(s.rollButton, vr::VROverlayFlags_SendVRDiscreteScrollEvents, true);
    std::snprintf(key, sizeof key, "frametop.screen.%d.resize", index + 1);
    std::snprintf(name, sizeof name, "Screen %d: resize", index + 1);
    s.handle = MakeChrome(key, name, corner, 64, 64);
    ApplyAlpha(s);
    // Until the layout places it: 2 m ahead of the head, in a row, screen 1 on the left.
    RefreshPoses();
    Mat head;
    if (!DevicePose(vr::k_unTrackedDeviceIndex_Hmd, &head)) head = Identity();
    const double heading = std::atan2(head.m[0][2], head.m[2][2]) * 180 / M_PI;
    const double yaw = heading + (double(count - 1) / 2 - index) * 35;
    const double dx = -std::sin(yaw * M_PI / 180), dz = -std::cos(yaw * M_PI / 180);
    SetAbsolute(s, PanelPose(head.m[0][3] + dx * 2, head.m[1][3], head.m[2][3] + dz * 2, yaw, 0, 0));
}

void ft_vr_screen_destroy(int index) {
    auto it = g_screens.find(index);
    if (it == g_screens.end()) return;
    for (auto o : it->second.All()) vr::VROverlay()->DestroyOverlay(o);
    g_screens.erase(it);
}

bool ft_vr_screen_present(int index, const void *key, const struct ft_dmabuf *b) {
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
        vr::HmdVector2_t scale = {float(s.width), float(s.height)};
        vr::VROverlay()->SetOverlayMouseScale(s.overlay, &scale);
        PlaceChrome(s);  // the height changed
        std::printf("screen %d: %dx%d\n", index + 1, s.width, s.height);
    }
    vr::SharedTextureHandle_t handle = it->second;
    vr::Texture_t tex = {&handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    vr::VROverlay()->SetOverlayTexture(s.overlay, &tex);
    s.shown = key;  // UpdateVisibility shows it on the next tick
    return true;
}

void ft_vr_forget(const void *key) {
    auto it = g_imports.find(key);
    if (it == g_imports.end()) return;
    vr::VRIPCResourceManager()->UnrefResource(it->second);
    g_imports.erase(it);
}

void ft_vr_poll(void (*handle)(const struct ft_event *, void *), void *data) {
    RefreshPoses();
    for (auto &[index, s] : g_screens) {
        vr::VREvent_t ev;
        // The screen itself: input for KWin.
        while (vr::VROverlay()->PollNextOverlayEvent(s.overlay, &ev, sizeof ev)) {
            ft_event e{};
            e.screen = index;
            switch (ev.eventType) {
                case vr::VREvent_MouseMove:
                    e.type = FT_MOTION;
                    e.x = ev.data.mouse.x;
                    e.y = s.height - ev.data.mouse.y;  // OpenVR's mouse origin is bottom left
                    break;
                case vr::VREvent_MouseButtonDown:
                case vr::VREvent_MouseButtonUp:
                    if (ev.eventType == vr::VREvent_MouseButtonUp) EndDragsBy(ev.trackedDeviceIndex);
                    e.type = FT_BUTTON;
                    e.button = LinuxButton(ev.data.mouse.button);
                    e.pressed = ev.eventType == vr::VREvent_MouseButtonDown;
                    e.x = ev.data.mouse.x;
                    e.y = s.height - ev.data.mouse.y;
                    break;
                case vr::VREvent_ScrollDiscrete:
                    e.type = FT_SCROLL;
                    e.dx = -ev.data.scroll.xdelta;
                    e.dy = -ev.data.scroll.ydelta;
                    break;
                case vr::VREvent_FocusLeave:
                    e.type = FT_LEAVE;
                    break;
                default:
                    continue;
            }
            handle(&e, data);
        }
        // The controls light up under a laser.
        auto hover = [&](int k) {
            const bool on = ev.eventType == vr::VREvent_MouseMove || ev.eventType == vr::VREvent_FocusEnter;
            if (!on && ev.eventType != vr::VREvent_FocusLeave) return;
            if (s.hover[k] != on) s.hover[k] = on, ApplyAlpha(s);
        };
        // The bar: move (and push/pull with the wheel while moving).
        while (vr::VROverlay()->PollNextOverlayEvent(s.bar, &ev, sizeof ev)) {
            hover(0);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Move, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp)
                EndDragsBy(ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_ScrollDiscrete && s.drag == Drag::Move)
                Push(s, ev.data.scroll.ydelta);
        }
        // The corner: resize.
        while (vr::VROverlay()->PollNextOverlayEvent(s.handle, &ev, sizeof ev)) {
            hover(3);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Resize, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp)
                EndDragsBy(ev.trackedDeviceIndex);
        }
        // The curve button.
        while (vr::VROverlay()->PollNextOverlayEvent(s.curveButton, &ev, sizeof ev)) {
            hover(1);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                ToggleCurve(s);
            else if (ev.eventType == vr::VREvent_MouseButtonUp)
                EndDragsBy(ev.trackedDeviceIndex);
        }
        // The roll button: drag around like a knob, or scroll.
        while (vr::VROverlay()->PollNextOverlayEvent(s.rollButton, &ev, sizeof ev)) {
            hover(2);
            if (ev.eventType == vr::VREvent_MouseButtonDown && ev.data.mouse.button == vr::VRMouseButton_Left)
                StartDrag(s, Drag::Roll, ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_MouseButtonUp)
                EndDragsBy(ev.trackedDeviceIndex);
            else if (ev.eventType == vr::VREvent_ScrollDiscrete && s.drag == Drag::None) {
                Mat p;
                if (!ScreenPose(s, &p)) continue;
                s.rollFrom = s.pinned != kNone ? s.pinRel : p;
                ApplyRoll(s, ev.data.scroll.ydelta * kRollStep * M_PI / 180);
            }
        }
        if (s.drag != Drag::None) UpdateDrag(s, index);
    }
    RefreshChrome();
    vr::VREvent_t ev;
    while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev)) {
        if (ev.eventType == vr::VREvent_Quit) {
            ft_event e{};
            e.type = FT_QUIT;
            handle(&e, data);
        }
        // A carrying controller that goes away drops its screen.
        if (ev.eventType == vr::VREvent_TrackedDeviceDeactivated) EndDragsBy(ev.trackedDeviceIndex);
    }
    ++g_tick;
    UpdateGame();
    UpdateArrange();
    UpdateVisibility();
    UpdateLasers();
    UpdateControls();
    UpdateGuides();
}

// Control commands (datagrams on @ft_screens, replies to the sender):
//   place <screen> <x> <y> <z> <yaw> <pitch> <roll>   centre (standing universe) and facing
//   width <screen> <metres>
//   curve <screen> <radius>   cylinder radius in metres; 0 = flat
//   curve <screen> on|off     on: the radius is the head's distance to it now -> "ok <radius>"
//   pin <screen|all> <left|right> [12 numbers]   pin to that hand's controller: as it is now,
//                             or at the given controller->screen transform (rows of a 3x4)
//   unpin <screen|all>
//   get <screen>  -> "ok x y z  xx xy xz  yx yy yz  zx zy zz  width height curve hand
//                     [12 numbers: controller->screen, when pinned]"
//   screens       -> "ok <count> <index>:<pixels w>x<h>:<metres> ..."
//   head          -> "ok x y z yaw"
//   visibility always|dashboard|gesture|toggle
//   wrist <degrees>           a pinned screen shows while you see its front within this
//   gesture <left|right> <degrees>   the gesture mode: look within this of that controller
//   hide | show | toggle      the manual switch (see g_manual)
//   controllers always|outside_games|dashboard   when controllers' lasers work the screens
//   ingames hide|visible      during a VR game, "always" acts like "only with the dashboard"
//                             (hide), or stays as it is (visible)
//   state         -> "ok <mode> <manual 0|1> <wrist deg> <gesture hand> <gesture deg>
//                     <controllers> <game running 0|1> <ingames>"
// (size <screen> <w> <h> and key <code> <value> are handled in compositor.c.) Screens are
// numbered from 1 here, like everywhere the user sees them.
void ft_vr_command(const char *cmd, char *reply, int size) {
    RefreshPoses();
    int n;
    double x, y, z, yaw, pitch, roll, w;
    char word[16], hand[16];
    float r[12];
    auto each = [&](const char *which, auto fn) -> bool {  // "all" or a screen number
        if (std::strcmp(which, "all") == 0) {
            for (auto &[i, s] : g_screens) fn(s);
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
        const vr::TrackedDeviceIndex_t dev = HandDevice(hand);
        Mat c;
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
        int len = std::snprintf(reply, size, "ok %zu", g_screens.size());
        for (auto &[i, s] : g_screens)
            if (len < size)
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
    } else if (!std::strncmp(cmd, "hide", 4) || !std::strncmp(cmd, "show", 4) || !std::strncmp(cmd, "toggle", 6)) {
        const bool always = EffectiveMode() == Mode::Always;
        const bool shownNow = always ? !g_manual : g_manual;
        const bool want = cmd[0] == 's' ? true : cmd[0] == 'h' ? false : !shownNow;
        g_manual = always ? !want : want;
        UpdateVisibility();
        std::snprintf(reply, size, "ok %s", want ? "shown" : "hidden");
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
    } else if (std::strncmp(cmd, "state", 5) == 0) {
        std::snprintf(reply, size, "ok %s %d %.0f %s %.0f %s %d %s", ModeName(), g_manual ? 1 : 0, g_wristAngle,
                      g_gestureHand.c_str(), g_gestureAngle, LasersName(), g_gameRunning ? 1 : 0,
                      g_inGames == InGames::Hide ? "hide" : "visible");
    } else {
        std::snprintf(reply, size, "error unknown command");
    }
}

}  // extern "C"
