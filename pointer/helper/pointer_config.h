// ft-pointer's settings, from ~/.config/frametop.conf. "The top" in comments here is the
// header comment of ft-pointer.cpp, which describes every setting.
#pragma once

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <string>
#include <vector>

inline std::map<std::string, std::string> ReadConfig() {
    std::map<std::string, std::string> conf;
    const char *home = std::getenv("HOME");
    std::ifstream in(std::string(home ? home : "") + "/.config/frametop.conf");
    std::string line;
    while (std::getline(in, line)) {
        line = line.substr(0, line.find('#'));
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
            return s;
        };
        conf[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return conf;
}

inline double ConfDouble(const std::map<std::string, std::string> &c, const char *key, double fallback) {
    auto it = c.find(key);
    return it == c.end() ? fallback : std::atof(it->second.c_str());
}

// POINTER_IGNORE: overlay keys the pointer passes through, as if they weren't there
// (display-only panels such as a performance overlay). Comma-separated shell patterns
// (fnmatch, no escapes), so "vendor.app*" covers an app's overlays.
inline std::vector<std::string> ParseIgnore(const std::string &list) {
    std::vector<std::string> out;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = std::min(list.find(',', at), list.size());
        std::string item = list.substr(at, comma - at);
        item.erase(0, item.find_first_not_of(" \t"));
        item.erase(item.find_last_not_of(" \t") + 1);
        if (!item.empty()) out.push_back(item);
        at = comma + 1;
    }
    return out;
}

struct PointerConfig {
    double freeDistance = 1.5, cursorDeg = 0.4, originFraction = 0.95, originMargin = 0.15, sceneRadius = 0.5,
           edgeReach = 0.3, grabOffset = 0.075,
           slideSpeed = 0.5, leashDeg = 10, leashReturn = 0.2, leashDelay = 0.2, followReach = 70;
    // POINTER_FOLLOW and POINTER_GAZE as read. They turn head follow and gaze mode on or off
    // only when they change, so a "follow" or "gaze" command wins until then (ApplyConfig in
    // ft-pointer.cpp).
    bool follow = false, gaze = false;
    double gazeRetake = 5, gazeNudgeMax = 55, gazeHold = 0.5, gazeShow = 1;
    bool gazeDotAlways = true;  // POINTER_GAZE_DOT (see the top)
    double headDeadzone = 0.5, keyTap = 0.25;  // POINTER_HEAD_DEADZONE, POINTER_KEY_TAP (keyboard clicks, see the top)
    // Hands (see the top): POINTER_HANDS, POINTER_PINCH_GAIN, POINTER_PINCH_DEADZONE, POINTER_GRIP_GAIN.
    bool handsOn = false;
    double pinchGain = 0.5, pinchDeadzone = 1.5, gripGain = 1.0, handBelow = 0.35, typingHold = 1.0;
    // Gaze precision (see the top): POINTER_GAZE_MOUSE (precision: the mouse's left button
    // holds back its press in gaze mode; direct: it clicks at once), POINTER_ROLE.
    bool gazeMousePrecision = true;
    bool gazeMouseHeld = true;  // POINTER_GAZE_MOUSE_MOVE (see the top)
    std::string role = "right";
    double pickupScale = 1;  // POINTER_CONTROLLER_PICKUP: scales the controller-moved limits
    std::vector<std::string> ignore;  // POINTER_IGNORE (see ParseIgnore)

    void Load(const std::map<std::string, std::string> &conf) {
        freeDistance = std::clamp(ConfDouble(conf, "POINTER_DISTANCE", 1.5), 0.3, 10.0);
        cursorDeg = std::clamp(ConfDouble(conf, "POINTER_CURSOR_DEG", 0.4), 0.05, 5.0);
        originFraction = std::clamp(ConfDouble(conf, "POINTER_ORIGIN_FRACTION", 0.95), 0.0, 0.98);
        originMargin = std::clamp(ConfDouble(conf, "POINTER_ORIGIN_MARGIN", 0.15), 0.0, 1.0);
        sceneRadius = std::clamp(ConfDouble(conf, "POINTER_SCENE_RADIUS", 0.5), 0.05, 2.0);
        edgeReach = std::clamp(ConfDouble(conf, "POINTER_EDGE_REACH", 0.3), 0.0, 2.0);
        grabOffset = std::clamp(ConfDouble(conf, "LAYOUT_GRAB_OFFSET", 0.075), 0.0, 1.0);
        slideSpeed = std::clamp(ConfDouble(conf, "LAYOUT_SLIDE_SPEED", 0.5), 0.02, 2.0);
        leashDeg = std::clamp(ConfDouble(conf, "POINTER_LEASH_DEG", 10), 0.0, 90.0);
        leashReturn = std::clamp(ConfDouble(conf, "POINTER_LEASH_RETURN", 0.2), 0.0, 5.0);
        leashDelay = std::clamp(ConfDouble(conf, "POINTER_LEASH_DELAY", 0.2), 0.0, 5.0);
        followReach = std::clamp(ConfDouble(conf, "POINTER_FOLLOW_REACH", 70), 10.0, 89.0);
        follow = ConfDouble(conf, "POINTER_FOLLOW", 0) != 0;
        gazeRetake = std::clamp(ConfDouble(conf, "POINTER_GAZE_RETAKE", 5), 1.0, 45.0);
        gazeNudgeMax = std::clamp(ConfDouble(conf, "POINTER_GAZE_NUDGE_MAX", 55), 1.0, 110.0);
        gazeHold = std::clamp(ConfDouble(conf, "POINTER_GAZE_HOLD", 0.5), 0.1, 5.0);
        gazeShow = std::clamp(ConfDouble(conf, "POINTER_GAZE_SHOW", 1), 0.0, 30.0);
        headDeadzone = std::clamp(ConfDouble(conf, "POINTER_HEAD_DEADZONE", 0.5), 0.0, 5.0);
        keyTap = std::clamp(ConfDouble(conf, "POINTER_KEY_TAP", 0.25), 0.0, 1.0);
        const auto gd = conf.find("POINTER_GAZE_DOT");
        gazeDotAlways = gd == conf.end() || gd->second != "moving";
        gaze = ConfDouble(conf, "POINTER_GAZE", 0) != 0;
        handsOn = ConfDouble(conf, "POINTER_HANDS", 0) != 0;
        pinchGain = std::clamp(ConfDouble(conf, "POINTER_PINCH_GAIN", 0.5), 0.05, 3.0);
        pinchDeadzone = std::clamp(ConfDouble(conf, "POINTER_PINCH_DEADZONE", 1.5), 0.0, 10.0);
        gripGain = std::clamp(ConfDouble(conf, "POINTER_GRIP_GAIN", 1.0), 0.05, 3.0);
        handBelow = std::clamp(ConfDouble(conf, "POINTER_GRIP_BELOW", 0.35), 0.05, 1.0);
        typingHold = std::clamp(ConfDouble(conf, "POINTER_PINCH_TYPING", 1.0), 0.0, 5.0);
        const auto gm = conf.find("POINTER_GAZE_MOUSE");
        gazeMousePrecision = gm == conf.end() || gm->second != "direct";
        const auto mm = conf.find("POINTER_GAZE_MOUSE_MOVE");
        gazeMouseHeld = mm == conf.end() || mm->second != "free";
        const auto ro = conf.find("POINTER_ROLE");
        role = ro != conf.end() && (ro->second == "left" || ro->second == "stylus") ? ro->second : "right";
        pickupScale = std::clamp(ConfDouble(conf, "POINTER_CONTROLLER_PICKUP", 1), 0.5, 5.0);
        const auto ig = conf.find("POINTER_IGNORE");
        ignore = ParseIgnore(ig == conf.end() ? "" : ig->second);
    }
};
