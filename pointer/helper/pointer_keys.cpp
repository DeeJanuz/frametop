// ft-pointer: keyboard clicks (see "Keyboard clicks" under "Gaze precision" at the top).
// "The top" in comments here is the header comment of ft-pointer.cpp.
#include "pointer.h"

// Where the head faces: yaw and pitch, degrees.
bool Pointer::HeadAngles(const vr::TrackedDevicePose_t &hmd, double &hy, double &hp) {
    const auto &hm = hmd.mDeviceToAbsoluteTracking.m;
    if (!hmd.bPoseIsValid) return false;
    const Vec3 f = Normalize({-hm[0][2], -hm[1][2], -hm[2][2]});
    hy = std::atan2(-f.x, -f.z) * 180 / M_PI;
    hp = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
    return true;
}

// Keyboard clicks (see the top): held back at the gaze, steered by the head.
void Pointer::KeyboardClicks(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto tnow = frame.tnow;
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
                EndHold(false);
                keyTilting = false;
            }
            continue;
        }
        if (k.right && hold.src == Src::Head && !hold.right && hold.pressed && leftHeld) {
            // gaze_right during a gaze_left drag: tilt while it's held (see the top).
            tilting = tiltStart = keyTilting = true;
            HeadAngles(hmd, keyTiltYaw, keyTiltPitch);
            if (debug) std::printf("hold key left: tilt began\n");
            if (debug) std::fflush(stdout);
            continue;
        }
        if (k.right && hold.src == Src::Head && !hold.right && !hold.pressed && aimHeld) {
            // gaze_right while gaze_left aims: press the left button where the dot is now
            // (the correction is a lesson), then the head drags.
            aimHeld = aimHand = false;
            hold.pressed = hold.grip = hold.keyDrag = true, hold.promote = false, hold.engaged = false;
            HeadAngles(hmd, hold.refYaw, hold.refPitch);
            gazeBack = gazeOn;
            pressRight = false;
            PressLeft();
            if (debug) std::printf("hold key left: pressed (right key)\n");
            if (debug) std::fflush(stdout);
            continue;
        }
        if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease) continue;
        Hold h;
        h.src = Src::Head, h.promote = true, h.right = k.right, h.pressYaw = yaw, h.pressPitch = pitch;
        if (!HeadAngles(hmd, h.refYaw, h.refPitch)) continue;
        hold = h;
        StartHold(false, tnow);
        if (debug) std::printf("hold key %s began\n", k.right ? "right" : "left");
        if (debug) std::fflush(stdout);
    }
    keyPresses.clear();
}

// A keyboard click's hold: the head steers the pointer, or turns the panel in a tilt.
void Pointer::HeadSteer(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto tnow = frame.tnow;
    if (hold.src == Src::Head) {
        double hy, hp;
        if (HeadAngles(hmd, hy, hp) && keyTilting) {
            // The head turns the panel, as the mouse does in a tilt; the pointer stays put.
            tiltYaw += std::remainder(hy - keyTiltYaw, 360.0);
            tiltPitch = std::clamp(tiltPitch + hp - keyTiltPitch, -80.0, 80.0);
            keyTiltYaw = hy, keyTiltPitch = hp;
        } else if (HeadAngles(hmd, hy, hp)) {
            Steer(hy, hp, hold.pressed ? 0.3 : cfg.headDeadzone, 1.0, tnow);
        }
    }
}

// A keyboard click held still for POINTER_GAZE_HOLD: a real press, then the head drags
// (from where it is now).
void Pointer::KeyboardClickHold(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto &hm = hmd.mDeviceToAbsoluteTracking.m;
    const auto tnow = frame.tnow;
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
        PressLeft();
        if (debug) std::printf("hold key %s: pressed (held still)\n", hold.right ? "right" : "left");
        if (debug) std::fflush(stdout);
    }
}
