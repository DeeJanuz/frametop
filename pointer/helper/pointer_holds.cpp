// ft-pointer: holds, hands, and the gaze precision buttons (see "Gaze precision" and "Hands" at
// the top).
// "The top" in comments here is the header comment of ft-pointer.cpp.
#include "pointer.h"

// Hands (see the top): pinches and grips from ft-hands.
void Pointer::Hands(const Frame &frame) {
    const auto tnow = frame.tnow;
    vr::TrackedDevicePose_t h0;
    sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &h0, 1);
    if (h0.bPoseIsValid) poses.Add(tnow, h0.mDeviceToAbsoluteTracking);
}

// Where a gesture's point is, seen from the hold's origin: yaw and pitch, degrees.
bool Pointer::HandAngles(const float p[3], uint64_t t_ns, const Vec3 &from, double &hy, double &hp) {
    vr::HmdMatrix34_t head;
    if (!poses.At(t_ns, head)) return false;
    const Vec3 d = Normalize(Position(head) + Rotate(head, Vec3{p[0], p[1], p[2]}) - from);
    hy = std::atan2(-d.x, -d.z) * 180 / M_PI;
    hp = std::asin(std::clamp(d.y, -1.0, 1.0)) * 180 / M_PI;
    return true;
}

void Pointer::EndHold(bool lost) {
    if (hold.pressed) {
        if (leftHeld) ReleaseLeft();
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
}

// The press, once a hold's source is set: a real press (dragging until the release), or
// held back like gaze mode's mouse press (see the top): the click comes on the release.
void Pointer::StartHold(bool press, Clock::time_point tnow) {
    hold.startYaw = hold.lastYaw = yaw, hold.startPitch = hold.lastPitch = pitch;
    hold.pressed = press;
    if (press) {
        gazeBack = gazeOn && gazeOwns;
        PressLeft();
    } else {
        gazeOwns = false;
        nudging = gazeOn && haveHead && tnow - gz.at < std::chrono::milliseconds(200);
        nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
        nudgeAt = aimSince = tnow, nudgeMoved = 0;
        aimHeld = aimHand = true;
        pulseAt = tnow;
    }
}

// The hold's source moved to (hy, hp): past the dead zone, the pointer follows at the gain,
// from where it was when it got past.
void Pointer::Steer(double hy, double hp, double deadzone, double gain, Clock::time_point tnow) {
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
}

void Pointer::BeginHold(int side, bool grip, const fh_pinch_t &g, Clock::time_point tnow) {
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
        Wake(tnow);  // the first gesture only wakes the pointer, like the first mouse move
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
    if (!HandAngles(g.begin_point, g.begin_ns, h.origin, h.refYaw, h.refPitch)) return;
    hold = h;
    // A grip, or a pinch without gaze mode: a real press where the pointer is (where
    // you look, in gaze mode), dragging with the hand until it opens.
    StartHold(grip || !gazeOn, tnow);
    if (debug) std::printf("hand %s %s began\n", side ? "right" : "left", grip ? "grip" : "pinch");
    if (debug) std::fflush(stdout);
}

// Gaze precision and gaze drag buttons (see the top): the mouse steers.
void Pointer::GazePrecisionButtons(const Frame &frame) {
    const auto tnow = frame.tnow;
    for (const DevicePress &p : devicePresses) {
        if (p.source == "left" || p.source == "right") continue;  // no controllers in gaze mode
        if (!p.down) {
            if (hold.src == Src::Mouse) EndHold(false);
            continue;
        }
        if (hold.src != Src::None || leftHeld || aimHeld || clickPress || clickRelease) continue;
        Hold h;
        h.src = Src::Mouse, h.grip = p.drag;
        hold = h;
        StartHold(p.drag, tnow);
        if (debug) std::printf("hold gaze %s began (%s)\n", p.drag ? "drag" : "precision", p.source.c_str());
        if (debug) std::fflush(stdout);
    }
    devicePresses.clear();
}

// Pinches and grips from ft-hands (see "Hands" at the top), read every frame.
void Pointer::PinchesAndGrips(Frame &frame) {
    const auto tnow = frame.tnow;
    fh_gestures_t hg;
    const bool handOk = cfg.handsOn && handFile.Read(hg);
    frame.handOk = handOk;
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
            if (hold.src == Src::Hand) EndHold(true);
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
                if (holding() && ended) EndHold(lost);  // the one held ended (another may have begun)
                if (began && allowed) {
                    BeginHold(s, k == 1, g, tnow);
                    // begun and ended since the last read (a quick tap): its release too
                    if (!(g.flags & FH_PINCH_DOWN) && holding()) EndHold(lost);
                }
            }
        // The held gesture's hand moves the pointer: past the dead zone (a tap's jitter,
        // the pinch point shifting as the fingers close), then at the gain, from there.
        if (hold.src == Src::Hand) {
            const fh_pinch_t &g = hold.grip ? hg.grip[hold.side] : hg.pinch[hold.side];
            double hy, hp;
            if ((g.flags & FH_PINCH_TRACKED) && HandAngles(g.point, hg.capture_ns, hold.origin, hy, hp))
                Steer(hy, hp, hold.grip ? 0.5 : cfg.pinchDeadzone, hold.grip ? cfg.gripGain : cfg.pinchGain, tnow);
        }
    }
}

// ft-hands stopped (or hung) with a gesture held: it's over, as lost.
void Pointer::HandsStopped(const Frame &frame) {
    const auto tnow = frame.tnow;
    const bool handOk = frame.handOk;
    const uint64_t nowNs =
        uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(tnow.time_since_epoch()).count());
    if (hold.src == Src::Hand && (!handOk || !active || nowNs - handPublished > 1'500'000'000ull)) EndHold(true);
    if (hold.src != Src::None && hold.src != Src::Hand && !active) EndHold(true);
}
