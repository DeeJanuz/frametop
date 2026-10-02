// ft-pointer: waking the pointer, its buttons, and the commands from the relay.
// "The top" in comments here is the header comment of ft-pointer.cpp.
#include "pointer.h"

void Pointer::Wake(Clock::time_point t) {
    if (t < noWakeUntil || headsetOff) return;
    wokeAt = t;
    active = true;
    recenter = true;
    SendTo(out, "ft_pointer", "role " + cfg.role);  // POINTER_ROLE, before it takes it
    SendTo(out, "ft_pointer", "show");
    claimPending = true;  // take the laser without clicking, once SteamVR has bound the device
    claimAt = t + std::chrono::milliseconds(300);
}

void Pointer::PressLeft() {
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
}

void Pointer::ReleaseLeft() {
    leftHeld = false;
    tilting = false;
    // Hold the drag pose (tilt, frozen distance) while SteamVR finishes the drop.
    dropHoldUntil = Clock::now() + std::chrono::milliseconds(500);
    SendTo(out, "ft_pointer", "btn " + heldButton + " 0");
    // ft-screens releases a button held on its screens in KWin even when SteamVR hands
    // the release to some other overlay (its catcher usually gets it; this is the backstop).
    SendTo(out, "ft_screens", "up");
    if (gazeBack) gazeOwns = true, gazeBack = false;
}

// The left button, from the relay's "btn trigger", with gaze mode's held-back press (see
// the top).
bool Pointer::CanAim() {
    return gazeOn && cfg.gazeMousePrecision && gazeOwns && !aimHeld && !clickPress && !clickRelease &&
           hold.src == Src::None;
}

// Hold the press back: the pointer stops where the gaze put it.
void Pointer::AimStart(bool right) {
    gazeOwns = false;
    nudging = haveHead && Clock::now() - gz.at < std::chrono::milliseconds(200);
    nudgeRawHy = gz.rhy, nudgeRawHp = gz.rhp, nudgeHead = lastHead;
    nudgeAt = aimSince = Clock::now(), nudgeMoved = 0;
    aimHeld = true, aimRight = right;
}

// A drag the right button began (chordDrag): it lasts while either button is held.
void Pointer::ChordDrop() {
    chordDrag = false;
    if (leftHeld) ReleaseLeft();
    if (debug) std::printf("mouse drag dropped\n");
    if (debug) std::fflush(stdout);
}

void Pointer::LeftButton(bool down) {
    leftDown = down;
    if (down) {
        if (aimHeld && aimRight && !aimHand) {
            ignoreLeftUp = true;  // the right's press is held back: the left does nothing
            return;
        }
        if (CanAim()) {
            AimStart(false);
            return;
        }
        PressLeft();
        return;
    }
    if (ignoreLeftUp) {
        ignoreLeftUp = false;
        return;
    }
    if (chordDrag) {
        if (!rightDown) ChordDrop();  // otherwise the right holds it (tilting, maybe)
        return;
    }
    if (aimHeld && !aimHand && !aimRight) {
        aimHeld = false;
        clickPress = true;  // after this frame's pose, so it lands where the pointer was moved to
        gazeBack = nudgeMoved < 0.2;
        return;
    }
    if (leftHeld) ReleaseLeft();
}

// The right button (see the top); false: not taken here (a tilt, or passed on as it is).
bool Pointer::RightButton(bool down) {
    rightDown = down;
    if (down) {
        if (aimHeld && !aimHand && !aimRight) {
            // During the left's held-back press: press the left where the pointer is now (the
            // correction is a lesson), and the mouse drags.
            aimHeld = false;
            chordDrag = gazeBack = true;
            PressLeft();
            if (debug) std::printf("mouse drag began (right during left)\n");
            if (debug) std::fflush(stdout);
            return true;
        }
        if (CanAim() && !leftHeld) {
            AimStart(true);
            return true;
        }
        return false;
    }
    if (chordDrag) {
        if (leftDown) return !swallowedRight;  // the left holds it; a tilt's release ends the tilt
        tilting = swallowedRight = false;
        ChordDrop();
        return true;
    }
    if (aimHeld && !aimHand && aimRight) {
        aimHeld = aimRight = false;
        pressRight = clickPress = true;  // the right click, where the pointer was moved to
        gazeBack = nudgeMoved < 0.2;
        return true;
    }
    if (leftHeld && heldButton == "b") {  // its press, held still into a real one
        ReleaseLeft();
        return true;
    }
    return false;
}

// POINTER_GAZE_MOUSE_MOVE=held (see the top): the gaze is fresh and nothing is pressed, so a
// mouse move doesn't move the pointer.
bool Pointer::MouseMoveHeld() {
    return gazeOn && cfg.gazeMouseHeld && !inGame && !headsetOff && Clock::now() - gz.at < std::chrono::seconds(1) &&
           !aimHeld && !leftHeld && !tilting && hold.src == Src::None && !clickPress && !clickRelease;
}

// Commands from the relay.
void Pointer::RelayCommands() {
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
        Command(buf, sender, senderLen);
    }
}

// One command from the relay (or another sender: reply answers it).
void Pointer::Command(const char *buf, const sockaddr_un &sender, socklen_t senderLen) {
    auto reply = [&](const std::string &msg) {
        if (senderLen > offsetof(sockaddr_un, sun_path))
            sendto(out, msg.data(), msg.size(), 0, reinterpret_cast<const sockaddr *>(&sender), senderLen);
    };
    // The gaze, from ft-gazed: not mouse input, it never wakes the pointer.
    double g[4];
    if (std::sscanf(buf, "gz %lf %lf %lf %lf", &g[0], &g[1], &g[2], &g[3]) == 4) {
        gz = {g[0], g[1], g[2], g[3], Clock::now()};
        return;
    }
    // The gaze calibration panel (see the top): presses answer it instead of clicking.
    {
        int on;
        if (std::sscanf(buf, "calpanel %d", &on) == 1) {
            const bool was = Clock::now() < calPanelUntil;
            calPanelUntil = on ? Clock::now() + std::chrono::seconds(3) : Clock::time_point{};
            if (on && !was) calOpened = true;
            return;
        }
    }
    if (Clock::now() < calPanelUntil) {
        const bool accept = !std::strncmp(buf, "btn trigger 1", 13) || !std::strncmp(buf, "gazekey left 1", 14);
        const bool quit = !std::strncmp(buf, "btn b 1", 7) || !std::strncmp(buf, "gazekey right 1", 15);
        if (accept || quit) {
            SendTo(out, "ft_gazed", accept ? "calaccept" : "calquit");
            return;
        }
        if (!std::strncmp(buf, "btn ", 4) || !std::strncmp(buf, "gazekey ", 8) ||
            !std::strncmp(buf, "precision ", 10) || !std::strncmp(buf, "gazedrag ", 9))
            return;  // their releases, and the other buttons: nothing to click now
    }
    {
        char kind[16], source[16];
        int v;
        if (std::sscanf(buf, "%15s %15s %d", kind, source, &v) == 3 &&
            (!std::strcmp(kind, "precision") || !std::strcmp(kind, "gazedrag"))) {
            lastMouse = Clock::now();
            if (!active) Wake(Clock::now());
            devicePresses.push_back({source, !std::strcmp(kind, "gazedrag"), v != 0});
            return;
        }
        if (std::sscanf(buf, "gazekey %15s %d", source, &v) == 2 &&
            (!std::strcmp(source, "left") || !std::strcmp(source, "right"))) {
            lastMouse = Clock::now();
            if (!active) Wake(Clock::now());
            keyPresses.push_back({!std::strcmp(source, "right"), v != 0});
            return;
        }
    }
    if (std::strcmp(buf, "typing") == 0) {  // not mouse input: it never wakes the pointer
        lastTyping = Clock::now();
        return;
    }
    if (std::strncmp(buf, "vrbind", 6) == 0) {
        std::printf("controller buttons: %s\n", controllerButtons.Bind(buf + 6).c_str());
        std::fflush(stdout);
        return;
    }
    if (std::strncmp(buf, "vrglobal", 8) == 0) {
        const char *arg = buf + 8;
        while (*arg == ' ') ++arg;
        ControllerButtons::SetGlobal(std::strncmp(arg, "off", 3) != 0);
        reply(controllerButtons.Status());
        return;
    }
    if (std::strncmp(buf, "vrstatus", 8) == 0) {
        reply(controllerButtons.Status());
        return;
    }
    if (std::strncmp(buf, "overlays", 8) == 0) {
        overlays.Request(sender, senderLen);  // answered from the list's thread
        return;
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
        return;
    }
    const bool mouseInput = std::strncmp(buf, "move", 4) == 0 || std::strncmp(buf, "btn", 3) == 0 ||
                            std::strncmp(buf, "scroll", 6) == 0;
    // A move held back (POINTER_GAZE_MOUSE_MOVE=held) only wakes the pointer: it isn't using
    // the mouse, so a drifting mouse doesn't keep the gaze from taking the pointer back.
    const bool moveHeldBack = std::strncmp(buf, "move", 4) == 0 && MouseMoveHeld();
    if (mouseInput && !moveHeldBack) lastMouse = Clock::now();
    // Any mouse input wakes the pointer (after a controller took over, or a helper restart).
    if (!active && mouseInput) Wake(Clock::now());
    if (moveHeldBack) return;
    double a, b;
    char key[128];
    double px, py, pz, pyaw, ppitch, proll = 0, pgrab = -1;
    if (std::sscanf(buf, "grabprobe %127s", key) == 1) {
        GrabProbe(key);
        return;
    }
    if (std::sscanf(buf, "place %127s %lf %lf %lf %lf %lf %lf %lf", key, &px, &py, &pz, &pyaw, &ppitch, &proll,
                    &pgrab) >= 6) {
        reply(Place(key, {px, py, pz}, PanelBasis(pyaw, ppitch, proll), pgrab >= 0 ? pgrab : cfg.grabOffset));
        return;
    }
    if (std::sscanf(buf, "measure %127s", key) == 1) {
        Panel p;
        Vec3 eye;
        const std::string err = FindPanel(key, p, eye);
        char msg[400] = "";
        if (err.empty())
            std::snprintf(msg, sizeof msg, "ok %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f",
                          p.center.x, p.center.y, p.center.z, p.width, p.height, p.basis.x.x, p.basis.x.y,
                          p.basis.x.z, p.basis.y.x, p.basis.y.y, p.basis.y.z, p.basis.z.x, p.basis.z.y,
                          p.basis.z.z);
        reply(err.empty() ? msg : "error " + err);
        return;
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
        return;
    }
    if (std::strncmp(buf, "debug", 5) == 0) {
        debug = !debug;
        std::printf("debug %s\n", debug ? "on" : "off");
        std::fflush(stdout);
        return;
    }
    if (std::strncmp(buf, "btn trigger 1", 13) == 0) {
        LeftButton(true);
        return;
    } else if (std::strncmp(buf, "btn trigger 0", 13) == 0) {
        LeftButton(false);
        return;
    } else if (std::strncmp(buf, "btn b 1", 7) == 0 && RightButton(true)) {
        return;
    } else if (std::strncmp(buf, "btn b 0", 7) == 0 && RightButton(false)) {
        return;
    } else if (std::strncmp(buf, "btn b 1", 7) == 0 && leftHeld) {
        tilting = tiltStart = swallowedRight = true;  // right press while dragging: tilt, no right-click
        return;
    } else if (std::strncmp(buf, "btn b 0", 7) == 0 && swallowedRight) {
        tilting = swallowedRight = false;
        return;
    }
    if (tilting && std::sscanf(buf, "move %lf %lf", &a, &b) == 2) {
        tiltYaw += a;
        tiltPitch = std::clamp(tiltPitch + b, -80.0, 80.0);
        return;
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
        if (!active) Wake(Clock::now());
    } else if (std::strncmp(buf, "hide", 4) == 0) {
        active = false;
        overlay->HideOverlay(cursor);
        overlay->HideOverlay(marker);
        SendTo(out, "ft_pointer", "hide");
    } else {
        SendTo(out, "ft_pointer", buf);  // btn, scroll
    }
}
