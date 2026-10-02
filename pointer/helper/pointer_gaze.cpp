// ft-pointer: gaze mode and its held-back press (see "Gaze mode" at the top).
// "The top" in comments here is the header comment of ft-pointer.cpp.
#include "pointer.h"

// Outside games, gaze mode keeps the pointer (see the top); the relay needs to know.
void Pointer::GazeAwake() {
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

// Gaze mode (see the top): the gaze has the pointer, or takes it back when you look
// well away from it. Not while a press holds the pointer, and only on fresh gaze.
void Pointer::GazeMode(const Frame &frame) {
    const vr::TrackedDevicePose_t &hmd = frame.Hmd();
    const auto tnow = frame.tnow;
    const Vec3 &eye = frame.eye;
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
}

void Pointer::CalPanelOpened() {
    if (calOpened) {  // the calibration panel came up: a press in progress ends, no click
        calOpened = false;
        if (hold.src != Src::None) EndHold(true);
        if (leftHeld) ReleaseLeft();
        aimHeld = aimHand = clickPress = false;
    }
}

// A held-back press (see the top): held still long enough, it's a real press (a drag);
// released, it's a click where the pointer is now (this frame's pose has gone out).
void Pointer::HeldBackPress(const Frame &frame) {
    const auto tnow = frame.tnow;
    if (!active) aimHeld = aimRight = clickPress = aimHand = confirmLesson = false;  // released meanwhile: nothing to click
    if (aimHeld && !aimHand && nudgeMoved < 0.2 && tnow - aimSince >= std::chrono::duration<double>(cfg.gazeHold)) {
        aimHeld = false;
        gazeBack = true;
        pressRight = aimRight;  // the right button's: a real right press (see the top)
        aimRight = false;
        PressLeft();
    }
}

// The click a held-back press ends in: the press now, the release 40 ms later.
void Pointer::Click(const Frame &frame) {
    const auto tnow = frame.tnow;
    if (clickPress) {
        clickPress = false;
        PressLeft();
        clickRelease = true;
        clickReleaseAt = tnow + std::chrono::milliseconds(40);
    } else if (clickRelease && tnow >= clickReleaseAt) {
        clickRelease = false;
        ReleaseLeft();
    }
}
