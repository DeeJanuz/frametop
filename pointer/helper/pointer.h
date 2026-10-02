// ft-pointer's shared code: helpers, the Frame, and the Pointer struct, whose member functions are
// in ft-pointer.cpp and pointer_*.cpp.
// "The top" in comments here is the header comment of ft-pointer.cpp.
#pragma once

#include <openvr.h>

#include "pointer_config.h"
#include "vrbuttons.h"
#include "vrmath.h"

extern "C" {
#include "../../hands/include/fh_gestures.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <climits>

#include <fcntl.h>
#include <fnmatch.h>

#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace md;

// Hand gestures from ft-hands (see "Hands" at the top): /run/user/UID/frametop-hands/gestures,
// mapped read-only. Version 1 files have pinches only; their grips read as zero.
class HandGestures {
public:
    ~HandGestures() { Close(); }
    // A consistent copy of the file (its sequence lock), if it's there. Opens it, and
    // checks it's still the same file, at most once a second.
    bool Read(fh_gestures_t &out) {
        const auto now = std::chrono::steady_clock::now();
        if (now - checked_ > std::chrono::seconds(1)) {
            checked_ = now;
            const std::string path = "/run/user/" + std::to_string(getuid()) + "/frametop-hands/gestures";
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || size_t(st.st_size) != len_ || st.st_ino != ino_) {
                Close();
                const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
                if (fd >= 0 && fstat(fd, &st) == 0 && size_t(st.st_size) >= offsetof(fh_gestures_t, grip)) {
                    void *m = mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_SHARED, fd, 0);
                    if (m != MAP_FAILED) map_ = m, len_ = size_t(st.st_size), ino_ = st.st_ino, ++opens;
                }
                if (fd >= 0) close(fd);
            }
        }
        if (!map_) return false;
        const auto *g = static_cast<const fh_gestures_t *>(map_);
        if (std::memcmp(g->magic, FH_GESTURES_MAGIC, 8) != 0) return false;
        const size_t n = std::min(len_, sizeof out);
        for (int tries = 0; tries < 3; ++tries) {
            const uint64_t seq = __atomic_load_n(&g->seq, __ATOMIC_ACQUIRE);
            if (seq & 1) continue;
            std::memset(&out, 0, sizeof out);
            std::memcpy(&out, map_, n);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (__atomic_load_n(&g->seq, __ATOMIC_RELAXED) != seq) continue;
            if (out.version < 2) std::memset(out.grip, 0, sizeof out.grip);
            return true;
        }
        return false;
    }
    int opens = 0;   // a new file: the counters start over

private:
    void Close() {
        if (map_) munmap(map_, len_);
        map_ = nullptr, len_ = 0, ino_ = 0;
    }
    void *map_ = nullptr;
    size_t len_ = 0;
    ino_t ino_ = 0;
    std::chrono::steady_clock::time_point checked_{};
};

// The HMD's recent poses (standing universe), to turn the gestures' head-frame points into
// the room as the head was when the cameras took them.
class PoseHistory {
public:
    void Add(std::chrono::steady_clock::time_point t, const vr::HmdMatrix34_t &m) {
        poses_.push_back({t, m});
        while (poses_.size() > 128) poses_.pop_front();   // about a second
    }
    // The pose at CLOCK_MONOTONIC time t_ns (steady_clock's), or the nearest kept.
    bool At(uint64_t t_ns, vr::HmdMatrix34_t &out) const {
        if (poses_.empty()) return false;
        const std::chrono::steady_clock::time_point t{std::chrono::nanoseconds(t_ns)};
        const auto *best = &poses_.front();
        for (const auto &p : poses_)
            if (std::chrono::abs(p.first - t) < std::chrono::abs(best->first - t)) best = &p;
        out = best->second;
        return true;
    }

private:
    std::deque<std::pair<std::chrono::steady_clock::time_point, vr::HmdMatrix34_t>> poses_;
};

// One of ft-screens' panels showing a desktop: a screen (frametop.screen.N), a floating
// window (frametop.float.N), or a floating window's popup (frametop.float.N.sub.K), not a
// control of theirs.
inline bool FramePanel(const std::string &key) {
    for (const char *prefix : {"frametop.screen.", "frametop.float."}) {
        if (key.rfind(prefix, 0) != 0) continue;
        const std::string rest = key.substr(std::strlen(prefix));
        const size_t dot = rest.find('.');
        return dot == std::string::npos || rest.compare(dot, 5, ".sub.") == 0;
    }
    return false;
}

inline void SendTo(int fd, const char *name, const std::string &msg) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, name, std::strlen(name));
    const socklen_t len = offsetof(sockaddr_un, sun_path) + 1 + std::strlen(name);
    sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr *>(&addr), len);
}

// JSON string literal (names come from other apps).
inline std::string JsonQuote(const std::string &s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += char(c);
        else if (c < 0x20) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\u%04x", c);
            out += esc;
        } else out += char(c);
    }
    return out + "\"";
}

// Overlay keys, refreshed in the background from `vrcmd --overlays` (OpenVR has no
// public call to enumerate other apps' overlays). Hidden ones are listed too: the
// window controls under a floating panel only appear while something hovers the
// panel, and the cursor has to find them the moment they do, not a second later.
// Paused while the pointer is off: each vrcmd run connects to SteamVR as a new app, and a new
// app every second kept SteamVR (and the headset's displays) from going to standby.
// "overlays" requests (Frametop Input Settings' Ignored panels page) refresh the list even
// while paused, and are answered from this thread once it's fresh.
class OverlayList {
public:
    void Start() {
        out_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        thread_ = std::thread([this] {
            while (running_) {
                if (!paused_ || requested_) Refresh();
                // Wait a second, or less when the pointer wakes (refresh right away then).
                for (int i = 0; i < 10 && running_; ++i) {
                    const bool wasPaused = paused_;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    if ((wasPaused && !paused_) || requested_) break;
                }
            }
        });
    }
    void SetPaused(bool paused) { paused_ = paused; }
    void Stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    std::vector<std::string> Keys() {
        std::lock_guard<std::mutex> guard(lock_);
        std::vector<std::string> keys;
        for (const auto &e : entries_) keys.push_back(e.key);
        return keys;
    }
    // Answer `to` with {"t":"overlays","list":[{"key","name","visible"}...]} after the next refresh.
    void Request(const sockaddr_un &to, socklen_t len) {
        if (len <= offsetof(sockaddr_un, sun_path)) return;
        std::lock_guard<std::mutex> guard(lock_);
        if (waiting_.size() < 8) waiting_.push_back({to, len});
        requested_ = true;
    }

private:
    struct Entry {
        std::string key, name;
        bool visible;
    };
    void Refresh() {
        requested_ = false;
        FILE *p = popen("LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64 /opt/steamvr/bin/linuxarm64/vrcmd --overlays 2>/dev/null", "r");
        if (!p) return;
        std::vector<Entry> entries;
        char line[1024];
        while (std::fgets(line, sizeof line, p)) {
            // 'key' -- 'name', WxH visible VROverlayType_...
            if (line[0] != '\'') continue;
            const char *end = std::strchr(line + 1, '\'');
            if (!end) continue;
            const std::string key(line + 1, size_t(end - (line + 1)));
            const std::string rest(end);
            if (rest.find("Thumbnail") != std::string::npos || rest.find("Subview") != std::string::npos) continue;
            if (key.rfind("system.pointer", 0) == 0 || key.rfind("system.cursor", 0) == 0 ||
                key.rfind("frametop.pointer", 0) == 0 || key.rfind("frametop.guide", 0) == 0 ||
                key == "frametop.gazepanel" ||  // the gaze calibration panel, fixed to the headset
                key == "frametop.catcher" ||  // ft-screens' release catcher: only on a laser mid-drag
                key == "system.HeadsetView" || key == "system.toast")
                continue;
            // The name can hold quotes; it ends at the last "', " (the size and state follow).
            const auto nameAt = rest.find("-- '"), nameEnd = rest.rfind("', ");
            const std::string name =
                nameAt != std::string::npos && nameEnd > nameAt + 3 ? rest.substr(nameAt + 4, nameEnd - nameAt - 4) : key;
            entries.push_back({key, name, rest.find(" not_visible ") == std::string::npos});
        }
        pclose(p);
        std::vector<std::pair<sockaddr_un, socklen_t>> waiting;
        {
            std::lock_guard<std::mutex> guard(lock_);
            entries_ = std::move(entries);
            waiting.swap(waiting_);
        }
        if (waiting.empty()) return;
        std::string msg = "{\"t\":\"overlays\",\"list\":[";
        for (size_t i = 0; i < entries_.size(); ++i)
            msg += std::string(i ? "," : "") + "{\"key\":" + JsonQuote(entries_[i].key) + ",\"name\":" +
                   JsonQuote(entries_[i].name) + ",\"visible\":" + (entries_[i].visible ? "true" : "false") + "}";
        msg += "]}";
        for (const auto &[to, len] : waiting)
            sendto(out_, msg.data(), msg.size(), MSG_DONTWAIT, reinterpret_cast<const sockaddr *>(&to), len);
    }

    std::thread thread_;
    int out_ = -1;
    std::atomic<bool> paused_{false};
    std::atomic<bool> running_{true};
    std::atomic<bool> requested_{false};
    std::mutex lock_;
    std::vector<Entry> entries_;  // written only by the thread; the lock guards readers
    std::vector<std::pair<sockaddr_un, socklen_t>> waiting_;
};

using Clock = std::chrono::steady_clock;

// One frame's poses and time: ReadFrame reads them after the relay's commands, and the sections
// after it share them. handOk: PinchesAndGrips read the hand gestures (for HandsStopped).
struct Frame {
    vr::TrackedDevicePose_t all[vr::k_unMaxTrackedDeviceCount];
    vr::TrackedDevicePose_t hmdRaw;
    Clock::time_point tnow;
    Vec3 eye;
    bool handOk = false;
    const vr::TrackedDevicePose_t &Hmd() const { return all[0]; }
};

// Everything the pointer keeps: its settings, what Init sets up, and the state the main loop
// carries from one frame to the next.
struct Pointer {
    PointerConfig cfg;
    // Head follow (see the top). followConf is POINTER_FOLLOW as last read: a reload only
    // overrides a "follow" command when the setting itself changed.
    bool follow = false, followConf = false, followReset = true;
    // Gaze mode (see the top); gazeConf is POINTER_GAZE as last read, like followConf.
    bool gazeOn = false, gazeConf = false;
    // Set in Init, after VR_Init.
    vr::IVRSystem *sys = nullptr;
    vr::IVROverlay *overlay = nullptr;
    // The dot, the marker, and laser mode's overlay, created in Init.
    vr::VROverlayHandle_t cursor = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t marker = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t laserMode = vr::k_ulOverlayHandleInvalid;
    bool laserModeShown = false;
    int in = -1, out = -1;  // @ft_pointer_helper, and the socket we send from (opened in Init)
    ControllerButtons controllerButtons;  // Frame controller buttons (vrbuttons.h)
    OverlayList overlays;
    std::map<std::string, vr::VROverlayHandle_t> handles;
    std::map<std::string, bool> sceneGraph;  // no texture: plane test instead of ComputeOverlayIntersection
    std::map<std::string, bool> visible;     // refreshed every 50 ms
    Clock::time_point lastVisible{};  // set in Init
    // The plane of the last panel the cursor was on, and the last point on it (panel edges).
    Vec3 edgePoint, edgeNormal, edgeLast;
    std::string edgeKey;

    bool active = false, recenter = false, anchored = false;
    Clock::time_point lastMouse{}, claimAt{}, claimRelease{}, wokeAt{}, noWakeUntil{};
    bool claimPending = false, claimHeld = false;
    // Last used wins: since when each controller has been moving (zero: it isn't).
    Clock::time_point movingSince[vr::k_unMaxTrackedDeviceCount] = {};
    // Tilt mode (see top of file).
    bool leftHeld = false, tilting = false, tiltStart = false, swallowedRight = false;
    // The mouse's buttons in gaze mode (see the top): leftDown, rightDown as the relay last said;
    // aimRight, the held-back press is the right button's; chordDrag, a drag the right button
    // began during the left's held-back press (the first release drops it, the other's is nothing).
    bool leftDown = false, rightDown = false, aimRight = false, chordDrag = false;
    bool ignoreLeftUp = false;
    // The keyboard clicks' keys as the relay last said, and a tilt gaze_right holds during a
    // keyboard drag (see the top): the head turns the panel, from where it was (keyTiltYaw, keyTiltPitch).
    bool keyLeftDown = false, keyRightDown = false, keyTilting = false;
    double keyTiltYaw = 0, keyTiltPitch = 0;
    double tiltYaw = 0, tiltPitch = 0;
    double dragDistance = 0, lastDistance = 1.5;  // drag lock: distance from the anchor at the press
    bool onVrSettings = false;       // the cursor is on the SteamVR Settings page (kept while dragging)
    bool catcherHidesHit = false;    // the laser-catching dot hides SteamVR's hit dot (Settings page)
    Clock::time_point dropHoldUntil{};  // after a left release: keep the drag pose this long
    bool debug = false;
    std::string lastHit;
    // The ft-screens panel the left button was pressed on, and where it was then (see the top).
    std::string pressKey;
    vr::HmdMatrix34_t pressPose{};
    Clock::time_point lastDebug{};  // set in Init
    vr::VROverlayHandle_t systemPointer = vr::k_ulOverlayHandleInvalid;  // found in Init
    Vec3 pivot, tiltOrigin, lastPoint, lastOrigin, lastAim{0, 0, -1};
    Basis tiltBasis{};
    bool headsetOff = false;  // nobody is wearing the headset (see the main loop)
    Vec3 anchor;
    double yaw = 0, pitch = 0;
    Vec3 followRef{0, 0, -1};  // head follow's reference direction (see the top)
    Clock::time_point followAt{};  // its last update, for the easing (set in Init)
    bool following = false;                           // past the leash: easing toward the head
    double followLag = 0;                             // radians the reference trails the head
    std::chrono::steady_clock::time_point leashOutSince{};  // head past the leash since (delay)
    // Gaze mode (see the top).
    struct Gaze {
        double hy = 0, hp = 0, rhy = 0, rhp = 0;  // corrected, and raw
        Clock::time_point at{};
    } gz;
    bool gazeOwns = true;  // the pointer follows the gaze; false: the mouse has it
    bool nudging = false;  // the mouse took it from the gaze: the next click may be a lesson
    double nudgeRawHy = 0, nudgeRawHp = 0, nudgeMoved = 0;
    vr::HmdMatrix34_t nudgeHead{}, lastHead{};
    bool haveHead = false, havePoint = false;
    Clock::time_point nudgeAt{}, retakeSince{};
    // A held-back press (see the top): aimHeld while the button is down; then the click
    // (clickPress at the end of the next frame, clickRelease 40 ms later). gazeBack: give
    // the gaze the pointer again when the press or click is over.
    vr::TrackedDeviceIndex_t ours = vr::k_unTrackedDeviceIndexInvalid;
    bool aimHeld = false, clickPress = false, clickRelease = false, gazeBack = false;
    bool aimHand = false;  // the held-back press is a hold's (below): it never turns into a real press
    Clock::time_point aimSince{}, clickReleaseAt{};
    // Holds (see "Gaze precision" and "Hands" at the top): a pinch or grip, or a gaze
    // precision or gaze drag button, held now. What steers the pointer meanwhile (a hand, or
    // the mouse through its own moves), from where it pointed when the hold began (for a
    // hand: seen from the eye then, in the room), and the pointer then.
    HandGestures handFile;
    PoseHistory poses;
    enum class Src { None, Hand, Mouse, Head };
    struct Hold {
        Src src = Src::None;
        int side = -1;          // a hand's side (0 left, 1 right)
        bool grip = false;      // pressed at once and dragging (a grip, gaze drag), not a click on release
        bool engaged = false;   // past the dead zone
        bool pressed = false;   // a real press went out (a grip or gaze drag, or a pinch without gaze mode)
        bool promote = false;   // held still for POINTER_GAZE_HOLD, it becomes a real press (keyboard clicks)
        bool right = false;     // the right button (gaze_right)
        bool keyDrag = false;   // gaze_right pressed while gaze_left aimed: a left press, either key ends it
        double pressYaw = 0, pressPitch = 0;  // the pointer at the press (a quick tap clicks there)
        Vec3 origin;
        double refYaw = 0, refPitch = 0, startYaw = 0, startPitch = 0, lastYaw = 0, lastPitch = 0;
    } hold;
    // "precision|gazedrag <source> 1|0" from the relay, done in the frame (see Holds).
    struct DevicePress {
        std::string source;
        bool drag, down;
    };
    std::vector<DevicePress> devicePresses;
    // "gazekey left|right 1|0" from the relay (keyboard clicks), done in the frame.
    struct KeyPress {
        bool right, down;
    };
    std::vector<KeyPress> keyPresses;
    uint32_t seenBegins[2][2] = {}, seenEnds[2][2] = {};  // [pinch, grip][side], as last read
    bool handBaseline = false;
    int handOpens = 0;
    uint64_t handSeq = 0, handPublished = 0;
    Clock::time_point handUsed{};  // a gesture began then (keeps the pointer, like gaze mode)
    Clock::time_point lastTyping{};  // the relay's last "typing": a key on a keyboard
    // Gaze mode outside games, and its dot (see the top): lastMove/lastHeld/pulseAt.
    bool inGame = false, gazeAwake = false;
    Clock::time_point inGameAt{}, gazeAwakeAt{};
    Clock::time_point lastMove{}, lastHeld{}, pulseAt{};
    // The left button, as sent to the driver; pressRight: the next press is the right button
    // instead (gaze_right), heldButton: the one pressed.
    bool pressRight = false;
    std::string heldButton = "trigger";
    bool confirmLesson = false;  // a keyboard click's quick tap: a lesson with no correction (see the top)
    // The gaze calibration panel is up until then ("calpanel 1"; see the top); calOpened: it just
    // came up, so a press in progress ends without a click.
    Clock::time_point calPanelUntil{};
    bool calOpened = false;
    Clock::time_point lastSlow{};  // set in Init

    void ApplyConfig();
    void Init();
    void Wake(Clock::time_point t);
    void PressLeft();
    void ReleaseLeft();
    bool CanAim();
    void AimStart(bool right);
    void ChordDrop();
    void LeftButton(bool down);
    bool RightButton(bool down);
    bool MouseMoveHeld();
    void SendPose(Vec3 originStanding, const Basis &b);
    std::pair<bool, Vec3> HeadPos();
    void Borrow();
    void GiveBack();
    std::string FindPanel(const char *key, Panel &p, Vec3 &eye);
    void GrabProbe(const char *key);
    std::string Place(const char *key, Vec3 target, const Basis &bt, double grabBelow);
    // Nearest's answer: the nearest overlay along a ray.
        struct Hit {
            double along = 1e9;
            std::string key;
            bool scene = false;
            Vec3 point, normal;
        };
    // Where the cursor is this frame (FindCursor), for DrawDot and SendRay.
    struct Spot {
        bool dragging = false;
        Vec3 dir, point;
        double best = 1e9, distance = 0;
        bool onEdge = false, occluded = false, onScene = false, onPanel = false;
    };

    // The main loop's sections, in the order main calls them, and what they share.
    void HeadsetOff();
    void GazeAwake();
    void RelayCommands();
    void Command(const char *buf, const sockaddr_un &sender, socklen_t senderLen);
    void ReadFrame(Frame &frame);
    void ClaimPulse(const Frame &frame);
    void PauseOverlayList();
    void LaserMode();
    void HandRole(const Frame &frame);
    void LastUsedWins(const Frame &frame);
    void Recenter(const Frame &frame);
    void GazeMode(const Frame &frame);
    void Hands(const Frame &frame);
    bool HandAngles(const float p[3], uint64_t t_ns, const Vec3 &from, double &hy, double &hp);
    void EndHold(bool lost);
    void StartHold(bool press, Clock::time_point tnow);
    void Steer(double hy, double hp, double deadzone, double gain, Clock::time_point tnow);
    void BeginHold(int side, bool grip, const fh_pinch_t &g, Clock::time_point tnow);
    void GazePrecisionButtons(const Frame &frame);
    bool HeadAngles(const vr::TrackedDevicePose_t &hmd, double &hy, double &hp);
    void KeyboardClicks(const Frame &frame);
    void CalPanelOpened();
    void HeadSteer(const Frame &frame);
    void PinchesAndGrips(Frame &frame);
    void HandsStopped(const Frame &frame);
    void MarkHeld(const Frame &frame);
    void HeadFollow(const Frame &frame);
    void SlowWork();
    void Cursor(const Frame &frame);
    void Tilt(const Frame &frame);
    Hit Nearest(Vec3 from, Vec3 d);
    Spot FindCursor(const Frame &frame);
    void DrawDot(const Frame &frame, const Spot &spot);
    void SendRay(const Frame &frame, const Spot &spot);
    void HeldBackPress(const Frame &frame);
    void KeyboardClickHold(const Frame &frame);
    void Click(const Frame &frame);
    void PollControllerButtons();
    bool SteamVRQuit();
};
