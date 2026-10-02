// ft-pointer: recenter, head follow, the overlay handles, and the cursor (see "Cursor model" at
// the top).
// "The top" in comments here is the header comment of ft-pointer.cpp.
#include "pointer.h"

namespace {

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

// A pending recenter (the command, a wake, a first move): the anchor goes to the eye, aimed where the
// head faces.
void Pointer::Recenter(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto &hm = hmd.mDeviceToAbsoluteTracking.m;
    const Vec3 &eye = frame.eye;
    if (recenter && hmd.bPoseIsValid) {
        anchor = eye;
        const Vec3 f{-hm[0][2], -hm[1][2], -hm[2][2]};
        yaw = std::atan2(-f.x, -f.z) * 180 / M_PI;
        pitch = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
        anchored = true;
        recenter = false;
        followReset = true;
    }
}

// Head follow (see the top): past the leash (for the delay), ease the reference to the
// head's facing, and turn the cursor with it. Not in gaze mode: the gaze places it.
void Pointer::HeadFollow(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto &hm = hmd.mDeviceToAbsoluteTracking.m;
    const auto tnow = frame.tnow;
    const Vec3 &eye = frame.eye;
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
}

// Slow work, once a second: overlay handles, our device index, laser width.
void Pointer::SlowWork() {
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
}

// The cursor: tilting a grabbed panel, or pointing (find the spot, draw the dot, send the ray).
void Pointer::Cursor(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    if (active && anchored && hmd.bPoseIsValid && tilting) {
        Tilt(frame);
    } else if (active && anchored && hmd.bPoseIsValid) {
        const Spot spot = FindCursor(frame);
        DrawDot(frame, spot);
        SendRay(frame, spot);
    }
}

void Pointer::Tilt(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const Vec3 &eye = frame.eye;
    const vr::TrackedDevicePose_t &hmdRaw = frame.hmdRaw;
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
}

// Nearest visible overlay along a ray (frozen while dragging).
Pointer::Hit Pointer::Nearest(Vec3 from, Vec3 d) {
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
}

// Where the cursor is this frame: on a panel, a scene-graph plane, or a panel's edge, or out in free
// space.
Pointer::Spot Pointer::FindCursor(const Frame &frame) {
    const auto tnow = frame.tnow;
    const Vec3 &eye = frame.eye;
    const bool dragging = leftHeld || tnow < dropHoldUntil;
    if (!dragging) tiltYaw = tiltPitch = 0;  // drop finished: back to plain pointing
    const Vec3 dir = Direction(yaw, pitch);
    Hit first;
    if (!dragging) first = Nearest(anchor, dir);
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
        const Hit front = Nearest(eye, Normalize(point - eye));
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
    Spot s;
    s.dragging = dragging, s.dir = dir, s.point = point, s.best = best, s.distance = distance;
    s.onEdge = onEdge, s.occluded = occluded, s.onScene = onScene, s.onPanel = onPanel;
    return s;
}

// Our dot where the cursor is (see "Looks" and "SteamVR Settings" at the top).
void Pointer::DrawDot(const Frame &frame, const Spot &spot) {
    const auto tnow = frame.tnow;
    const Vec3 &eye = frame.eye;
    const bool dragging = spot.dragging, onScene = spot.onScene, onPanel = spot.onPanel;
    const Vec3 dir = spot.dir, point = spot.point;
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
}

void Pointer::SendRay(const Frame &frame, const Spot &spot) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto tnow = frame.tnow;
    const Vec3 &eye = frame.eye;
    const vr::TrackedDevicePose_t &hmdRaw = frame.hmdRaw;
    const bool dragging = spot.dragging, occluded = spot.occluded, onEdge = spot.onEdge, onScene = spot.onScene;
    const double best = spot.best, distance = spot.distance;
    const Vec3 point = spot.point;
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
