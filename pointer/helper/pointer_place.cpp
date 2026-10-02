// ft-pointer: panel placement for layouts (see "Placement" at the top).
// "The top" in comments here is the header comment of ft-pointer.cpp.
#include "pointer.h"

namespace {

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

}  // namespace

// Placement speeds (see "Placement" at the top).
constexpr double kPlaceDegPerSec = 60;      // tested: 40 deg/s is applied exactly

// --- Panel placement (see "Placement" at the top of the file) ---
// Device pose, given in the standing universe, sent to the driver in raw space.
void Pointer::SendPose(Vec3 originStanding, const Basis &b) {
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
}

std::pair<bool, Vec3> Pointer::HeadPos() {
    vr::TrackedDevicePose_t s;
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &s, 1);
    return std::make_pair(s.bPoseIsValid, Position(s.mDeviceToAbsoluteTracking));
}

// Borrow the device and the laser for a placement: connect, claim, laser mode on.
void Pointer::Borrow() {
    SendTo(out, "ft_pointer", "show");
    overlay->ShowOverlay(laserMode);
    overlay->HideOverlay(cursor);
    overlay->HideOverlay(marker);
    SleepMs(active ? 50 : 400);  // a fresh connect needs SteamVR to bind the device
    SendTo(out, "ft_pointer", "btn a 1");
    SleepMs(60);
    SendTo(out, "ft_pointer", "btn a 0");
}

void Pointer::GiveBack() {
    if (!active) {
        SendTo(out, "ft_pointer", "hide");
        overlay->HideOverlay(laserMode);
        laserModeShown = false;
    }
}

std::string Pointer::FindPanel(const char *key, Panel &p, Vec3 &eye) {
    vr::VROverlayHandle_t h;
    if (overlay->FindOverlay(key, &h) != vr::VROverlayError_None) return std::string("no overlay ") + key;
    bool valid;
    std::tie(valid, eye) = HeadPos();
    if (!valid) return "no head pose (headset off?)";
    p = ScanPanel(h, eye, 0.5);
    if (!p.found) return std::string("panel not visible: ") + key;
    return "";
}

// grabprobe (maintenance): aim down from the panel's bottom edge, 1 cm a step, and log where
// SteamVR's laser hits something (the hit dot shows) and whether the window controls are up.
void Pointer::GrabProbe(const char *key) {
    Panel p;
    Vec3 eye;
    const std::string err = FindPanel(key, p, eye);
    if (!err.empty()) {
        std::printf("grabprobe: %s\n", err.c_str());
        std::fflush(stdout);
        return;
    }
    Borrow();
    vr::VROverlayHandle_t footer = vr::k_ulOverlayHandleInvalid;
    overlay->FindOverlay("valve.steam.gamepadui.floatingfooter", &footer);
    const Vec3 bottom = p.center - p.basis.y * (p.height / 2);
    std::printf("grabprobe %s: center (%.3f %.3f %.3f) %.3f x %.3f m\n", key, p.center.x, p.center.y, p.center.z,
                p.width, p.height);
    for (int cm = -5; cm <= 45; ++cm) {
        const Vec3 target = bottom - p.basis.y * (cm / 100.0);
        SendPose(eye, AimBasis(target - eye));
        SleepMs(90);
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
    GiveBack();
}

// Carry a floating panel so its centre lands on `target` with frame `bt` (see
// "Placement" at the top). Returns "ok ..." or "error ...".
std::string Pointer::Place(const char *key, Vec3 target, const Basis &bt, double grabBelow) {
    Panel p;
    Vec3 eye;
    std::string err = FindPanel(key, p, eye);
    if (!err.empty()) return "error " + err;
    auto offBy = [&](const Panel &q, double &cm, double &deg) {
        cm = Length(q.center - target) * 100;
        const double c = (Dot(q.basis.x, bt.x) + Dot(q.basis.y, bt.y) + Dot(q.basis.z, bt.z) - 1) / 2;
        deg = std::acos(std::clamp(c, -1.0, 1.0)) * 180 / M_PI;
    };
    double cm, deg;
    int moves = 0;
    Borrow();
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
        SendPose(eye, d0);
        SleepMs(150);  // hover: the window controls come up
        SendTo(out, "ft_pointer", "btn trigger 1");
        SleepMs(150);
        // 1. Rotate about the device origin (the eye): the dashboard applies it exactly.
        const int rsteps = std::max(4, int(angle * 180 / M_PI / kPlaceDegPerSec * 60));
        for (int i = 1; i <= rsteps; ++i) {
            const double a = angle * i / rsteps;
            auto r = [&](Vec3 v) { return RotateAbout(v, axis, a); };
            SendPose(eye, {r(d0.x), r(d0.y), r(d0.z)});
            SleepMs(16);
        }
        // 2. Slide the device slowly: fast moves are accelerated by the dashboard.
        const Basis d1{turn(d0.x), turn(d0.y), turn(d0.z)};
        const int tsteps = std::max(4, int(Length(o1 - eye) / cfg.slideSpeed * 60));
        for (int i = 1; i <= tsteps; ++i) {
            SendPose(eye + (o1 - eye) * (double(i) / tsteps), d1);
            SleepMs(16);
        }
        SleepMs(100);
        SendTo(out, "ft_pointer", "btn trigger 0");
        SleepMs(600);  // the dashboard re-reads the pose up to 150 ms after the release
        err = FindPanel(key, p, eye);
        if (!err.empty()) break;
    }
    GiveBack();
    if (!err.empty()) return "error after the move: " + err;
    offBy(p, cm, deg);
    char msg[160];
    std::snprintf(msg, sizeof msg, "ok %s off by %.1f cm, %.1f deg after %d move%s", key, cm, deg, moves,
                  moves == 1 ? "" : "s");
    std::printf("place: %s\n", msg);
    std::fflush(stdout);
    return msg;
}
