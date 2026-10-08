// ft-gaze: the headset's eye tracking as rays and Frametop screen pixels (OpenVR overlay
// client, runs in the dev container). The gaze service (ft-gazed) and the gaze probe run it.
//
// Every eye tracker sample (90 Hz) becomes one JSON line on stdout with each gaze source
// hit-tested against the Frametop screens:
//
// Options: -v (log action errors), --watch-stdin (quit when stdin closes; until then, a line
// "sources LIST" on stdin switches the sources as --sources does), --sources LIST (comma-
// separated: action, mmap1, mmap2, left, right, own, and eye for EYE; or all, the default).
// Sources left out are read not at all and print as {"ok":0} ("eye" as null), so every line
// keeps the same keys. The gaze service asks for the ones it uses (own and mmap1 with our
// tracker), and all of them while a check or the calibration runs. Only the action costs
// SteamVR anything (two calls into vrserver per sample), and a line with every source is
// about 1.5 KB, 130 KB a second through podman's relay.
//
//   {"t":<sample time, CLOCK_MONOTONIC_RAW s>,"age":<ms old when read>,"n":<sample counter>,
//    "head":{"yaw":..,"pitch":..,"hit":HIT},          head forward ray (for head nudging)
//    "src":{"action":SRC,"mmap1":SRC,"mmap2":SRC,"left":SRC,"right":SRC,"own":SRC},"eye":EYE}
//   SRC = {"hy":..,"hp":..,"hit":HIT} or {"ok":0}    hy/hp: gaze direction relative to the
//                                                     head, degrees (yaw +left, pitch +up)
//         mmap1 adds "open":[l,r] (probably eye openness, 0 in a blink) and "dist" (vergence
//         distance, m); both mmap sets add "lr", the angle between the eyes (deg), which
//         jumps when the tracker loses an eye, and "eyes":[[hy,hp],[hy,hp]], each eye's own
//         direction (left, right), for calibrating the eyes separately, and "unc":[l,r],
//         the tracker's uncertainty about each eye's direction (its filter's variance):
//         about 0.0005-0.002 while it sees the eye, 0.015-0.03 once it's lost it.
//    "left":SRC,"right":SRC                           each eye's own direction from set 2
//         (set 1's eyes always share one pitch, and while it's lost an eye it keeps that
//         eye's yaw where it was: set 2 is each eye's own reading). From the head's origin,
//         not the eye's.
//    "own":SRC                                        our own tracker (gaze/tracker/ft-eyes), from
//         /dev/shm/frametop-eyes-gaze; adds "age" (ms since its frame), "eyes":[[hy,hp],[hy,hp]]
//         (left, right; null for an eye it doesn't see), "ehit":[HIT,HIT] where each of those
//         lands, and "slip":[[x,y],[x,y]] (left, right: each eye's shift in its camera image
//         since the calibration, pixels; null until a click has measured it). {"ok":0}
//         without the file or when it's over 100 ms old.
//   EYE = {"q":[l,r],"m":[[x,y],[x,y]],"new":[l,r]}  the tracker's latest measurement of
//         each eye before filtering: "m" (camera-relative, undocumented units), "q" its
//         variance (about 2e-5 on a clear view of the eye, rising as the lid or lashes get
//         in the way), "new" whether it changed since the last sample (it freezes while the
//         tracker can't see that eye, and in blinks). "eye" is null without the mmap.
//   HIT = {"s":<screen>,"x":..,"y":..,"j":[dx/dhy,dy/dhy,dx/dhp,dy/dhp],"dpp":<deg per px>}
//         or null. x, y are pixels on that screen; j is pixels per degree of head-relative
//         yaw and pitch there, so a correction in degrees can be turned into pixels and back.
//
// Sources:
//   action  SteamVR input: an "eyetracking" action bound to /user/head/eyetracking, read
//           with IVRInput::GetEyeTrackingDataRelativeToNow. The supported way.
//   mmap1/2 /dev/shm/eye-server.mmap, written by SteamVR's eyetracking process for the HMD
//           driver. Undocumented; the layout below was worked out by reading it and can
//           change with any SteamVR update. Two sets of per-eye directions in head space
//           (-Z forward); which one has SteamVR's per-user calibration applied is what the
//           probe is for. Opened read-only: the other half of the file carries calibration
//           clicks to the eye tracker, and must never be written.
//
// The mmap samples are in head space, 17 ms or so old when they appear, so each is turned
// into the room with the head pose at its own timestamp, from a short pose history.
//
// Screens come from ft-screens (@ft_screens: "screens", "remotes" for other machines'
// displays, "get N"), refreshed 4 times a second in the background. A curved screen is a cylinder toward its front (see OnSurface
// in screens/vr.cpp).
#include <openvr.h>

#include "vrmath.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {

using namespace md;

double NowRaw() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// --- eye-server.mmap (packed, unaligned: read with memcpy) ---
// The SteamOS 0.4.x beta (SteamVR 2.18.2) moved every field from the timestamp on by 5
// bytes (measured 2026-10-04 with the ftdiag scan: timestamp 0x157 -> 0x15c, the vectors
// moved with it; the counter at 0x38 kept its place). Which layout is live is detected at
// runtime (EyeFile::Detect), so one binary serves both generations.
constexpr size_t kCounter = 0x38;  // u32, one per sample
constexpr size_t kTime = 0x157;    // f64, CLOCK_MONOTONIC_RAW seconds
constexpr size_t kLeft1 = 0x15f, kRight1 = 0x16b;  // set 1: unit vectors, head space
constexpr size_t kFix1 = 0x18f;    // set 1 fixation point: length is the vergence distance (m)
constexpr size_t kLeft2 = 0x19b, kRight2 = 0x1a7;  // set 2
constexpr size_t kOpen = 0x1cb;    // two floats, 0..1: probably eye openness or confidence
// After each set's two directions, six floats: the left eye's variance (three), the
// right's (three; the middle one of each is shared). They jump when an eye is lost.
constexpr size_t kVar1 = 0x177, kVar2 = 0x1b3;
// The measurements the filter is fed: left x, y, right x, y, then the variance of each (left
// x, y, right x, y). An eye's pair stops changing while the tracker can't see it.
constexpr size_t kMeas = 0x1d3;
constexpr size_t kNeed = 0x1f3 + 5;  // enough for either layout
constexpr size_t kShifts[] = {0, 5};  // the layouts EyeFile::Detect knows: stable, 0.4.x beta

struct EyeFile {
    // Everything from the timestamp on is read at base + shift: 0 on stable, 5 on the
    // 0.4.x beta (see the constants above). known once Detect has seen that layout's
    // timestamp tick; before that, reading would yield garbage that still passes
    // ReadSample's check.
    size_t shift = 0;
    bool known = false;
    const uint8_t *p = nullptr;
    size_t size = 0;
    size_t At(size_t base) const { return base + shift; }
    bool Open() {
        const int fd = open("/dev/shm/eye-server.mmap", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return false;
        struct stat st {};
        if (fstat(fd, &st) != 0 || size_t(st.st_size) < kNeed) {
            close(fd);
            return false;
        }
        void *m = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        close(fd);
        if (m == MAP_FAILED) return false;
        p = static_cast<const uint8_t *>(m);
        size = st.st_size;
        return true;
    }
    // The sample counter, or 0 without the mapping: the one read the loop makes whether or
    // not the file was there (Get, V and ReadSample need it).
    uint32_t Counter() const { return p ? Get<uint32_t>(kCounter) : 0; }
    template <class T> T Get(size_t off) const {
        T v;
        std::memcpy(&v, p + off, sizeof v);
        return v;
    }
    Vec3 V(size_t off) const {
        float f[3];
        std::memcpy(f, p + off, sizeof f);
        return {f[0], f[1], f[2]};
    }
    // A live timestamp is near the clock it comes from (its samples are 17 ms or so old).
    static bool TimePlausible(double t, double now) { return t > now - 2.0 && t <= now + 2.0; }
    // The eye directions are unit vectors, so their length is a second, independent check
    // next to the timestamp: a mere coincidence in one field does not confirm a layout.
    static bool UnitVec(Vec3 v) {
        const float n = v.x * v.x + v.y * v.y + v.z * v.z;
        return n > 0.81f && n < 1.21f;  // |v| within 0.9 .. 1.1
    }
    bool Fits(size_t layout, double now) const {
        return TimePlausible(Get<double>(kTime + layout), now) && UnitVec(V(kLeft1 + layout)) &&
               UnitVec(V(kRight1 + layout));
    }

    // Which layout is live, a step per pass of the loop, so it never stalls it. A layout
    // fits when its timestamp is near the clock and its two set-1 directions are unit
    // vectors; it's taken once its timestamp has moved on too, within kConfirm. That allows
    // for 2 samples a second: the eye server writes 72 or 90 (15 were seen on the beta).
    // kWaiting: call again on the next pass. kNone: no layout fits (a server that stopped,
    // SteamVR's tracker warming up, or a layout we don't know), so the mmap stays unused;
    // try again later. One detection per run is enough: the layout can't change under a
    // running ft-gaze, since SteamVR starts the eye server that writes the file, and the
    // gaze service stops and starts with SteamVR.
    enum Detection { kWaiting, kFound, kNone };
    static constexpr double kConfirm = 0.5;
    static constexpr size_t kLayouts = sizeof kShifts / sizeof kShifts[0];
    Detection Detect(double now) {
        if (!fit_) {
            for (size_t i = 0; i < kLayouts; ++i)
                if (Fits(kShifts[i], now)) fit_ |= 1u << i, t0_[i] = Get<double>(kTime + kShifts[i]);
            if (!fit_) return kNone;
            since_ = now;
            return kWaiting;
        }
        for (size_t i = 0; i < kLayouts; ++i)
            if ((fit_ >> i & 1) && Get<double>(kTime + kShifts[i]) > t0_[i] && Fits(kShifts[i], now)) {
                fit_ = 0, shift = kShifts[i], known = true;
                return kFound;
            }
        if (now - since_ <= kConfirm) return kWaiting;
        fit_ = 0;
        return kNone;
    }
    bool Detecting() const { return fit_ != 0; }

    // Detect's state: the layouts that fit at since_ (a bit each), and their timestamps then.
    unsigned fit_ = 0;
    double t0_[kLayouts] = {}, since_ = 0;
};

struct EyeSample {
    uint32_t n = 0;
    double t = 0;
    Vec3 left1, right1, fix1, left2, right2;
    float open[2] = {0, 0};
    float var1[6] = {}, var2[6] = {}, meas[8] = {};
};

// A consistent copy: the writer has no seqlock we can use, so read until the counter and
// timestamp are the same before and after. Only call this once the layout is known
// (EyeFile::known): on an unknown layout these reads still pass the consistency check,
// but yield garbage.
bool ReadSample(const EyeFile &f, EyeSample &s) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const uint32_t n0 = f.Get<uint32_t>(kCounter);
        const double t0 = f.Get<double>(f.At(kTime));
        std::atomic_thread_fence(std::memory_order_acquire);
        s.left1 = f.V(f.At(kLeft1)), s.right1 = f.V(f.At(kRight1)), s.fix1 = f.V(f.At(kFix1));
        s.left2 = f.V(f.At(kLeft2)), s.right2 = f.V(f.At(kRight2));
        std::memcpy(s.open, f.p + f.At(kOpen), sizeof s.open);
        std::memcpy(s.var1, f.p + f.At(kVar1), sizeof s.var1);
        std::memcpy(s.var2, f.p + f.At(kVar2), sizeof s.var2);
        std::memcpy(s.meas, f.p + f.At(kMeas), sizeof s.meas);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (f.Get<uint32_t>(kCounter) == n0 && f.Get<double>(f.At(kTime)) == t0) {
            s.n = n0, s.t = t0;
            return true;
        }
    }
    return false;
}

// --- Our own tracker: /dev/shm/frametop-eyes-gaze, written by gaze/tracker/ft-eyes ---
// Layout (ft-eyes' docstring): u32 seq (odd while written), u32 version, f64 t, f32 yaw,
// pitch, u32 flags (bit 0 right eye, 1 left, 2 right slip known, 3 left), u32 n, then f32
// right yaw, pitch, left yaw, pitch; slip right x, y, left x, y; pupils (unused here).
struct OwnSample {
    double t = 0;
    float yaw = 0, pitch = 0;
    uint32_t flags = 0, n = 0;
    float eyes[4] = {}, slip[4] = {};
};

class OwnFile {
public:
    // Reopened when it appears or is replaced, since ft-eyes may start after us.
    bool Read(OwnSample &o) {
        const double now = NowRaw();
        if (!p_ || now - checked_ > 2.0) Reopen(now);
        if (!p_) return false;
        for (int attempt = 0; attempt < 4; ++attempt) {
            uint32_t s0, s1, version;
            std::memcpy(&s0, p_, 4);
            if (s0 & 1) continue;
            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(&version, p_ + 4, 4);
            std::memcpy(&o.t, p_ + 8, 8);
            std::memcpy(&o.yaw, p_ + 16, 4);
            std::memcpy(&o.pitch, p_ + 20, 4);
            std::memcpy(&o.flags, p_ + 24, 4);
            std::memcpy(&o.n, p_ + 28, 4);
            std::memcpy(o.eyes, p_ + 32, sizeof o.eyes);
            std::memcpy(o.slip, p_ + 48, sizeof o.slip);
            std::atomic_thread_fence(std::memory_order_acquire);
            std::memcpy(&s1, p_, 4);
            if (s0 == s1) return version == 1;
        }
        return false;
    }

private:
    static constexpr size_t kSize = 128;
    void Reopen(double now) {
        checked_ = now;
        struct stat st {};
        if (stat("/dev/shm/frametop-eyes-gaze", &st) != 0) return Close();
        if (p_ && st.st_ino == ino_) return;
        Close();
        const int fd = open("/dev/shm/frametop-eyes-gaze", O_RDONLY | O_CLOEXEC);
        if (fd < 0) return;
        if (fstat(fd, &st) == 0 && size_t(st.st_size) >= kSize) {
            void *m = mmap(nullptr, kSize, PROT_READ, MAP_SHARED, fd, 0);
            if (m != MAP_FAILED) p_ = static_cast<const uint8_t *>(m), ino_ = st.st_ino;
        }
        close(fd);
    }
    void Close() {
        if (p_) munmap(const_cast<uint8_t *>(p_), kSize);
        p_ = nullptr;
    }
    const uint8_t *p_ = nullptr;
    ino_t ino_ = 0;
    double checked_ = -1e9;
};

// --- Screens from ft-screens ---
struct Screen {
    int index = 0;
    int wpx = 0, hpx = 0;
    double metres = 0, height = 0, curve = 0;
    Vec3 c;
    Basis b;
};

class Screens {
public:
    void Start() {
        thread_ = std::thread([this] {
            const int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
            sockaddr_un me{};
            me.sun_family = AF_UNIX;
            const std::string name = "ft_gaze." + std::to_string(getpid());
            std::memcpy(me.sun_path + 1, name.data(), name.size());
            bind(fd, reinterpret_cast<sockaddr *>(&me), offsetof(sockaddr_un, sun_path) + 1 + name.size());
            timeval tv{0, 200000};
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            while (running_) {
                std::vector<Screen> got;
                Query(fd, got);
                {
                    std::lock_guard<std::mutex> guard(lock_);
                    screens_ = std::move(got);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            close(fd);
        });
    }
    void Stop() {
        running_ = false;
        if (thread_.joinable()) thread_.join();
    }
    std::vector<Screen> Get() {
        std::lock_guard<std::mutex> guard(lock_);
        return screens_;
    }

private:
    static std::string Ask(int fd, const std::string &cmd) {
        sockaddr_un to{};
        to.sun_family = AF_UNIX;
        const char name[] = "ft_screens";
        std::memcpy(to.sun_path + 1, name, sizeof name - 1);
        sendto(fd, cmd.data(), cmd.size(), 0, reinterpret_cast<sockaddr *>(&to),
               offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1);
        char buf[1024];
        const ssize_t n = recv(fd, buf, sizeof buf - 1, 0);
        if (n <= 0) return "";
        buf[n] = 0;
        return buf;
    }
    static void Query(int fd, std::vector<Screen> &out) {
        // "ok <count> <index>:<w>x<h>:<metres> ..."
        const std::string list = Ask(fd, "screens");
        if (list.rfind("ok ", 0) != 0) return;
        const char *p = list.c_str() + 3;
        int count = 0, used = 0;
        if (std::sscanf(p, "%d%n", &count, &used) != 1) return;
        p += used;
        for (int k = 0; k < count; ++k) {
            Screen s;
            if (std::sscanf(p, " %d:%dx%d:%lf%n", &s.index, &s.wpx, &s.hpx, &s.metres, &used) != 4) break;
            p += used;
            if (Place(fd, s)) out.push_back(s);
        }
        // Other machines' displays (remote.c): "ok <count> <index>:<client>:<state>:<w>x<h> ..."
        const std::string remotes = Ask(fd, "remotes");
        if (remotes.rfind("ok ", 0) != 0) return;
        p = remotes.c_str() + 3;
        if (std::sscanf(p, "%d%n", &count, &used) != 1) return;
        p += used;
        for (int k = 0; k < count; ++k) {
            Screen s;
            if (std::sscanf(p, " %d:%*[^:]:%*[^:]:%dx%d%n", &s.index, &s.wpx, &s.hpx, &used) != 3) break;
            p += used;
            if (s.wpx > 0 && s.hpx > 0 && Place(fd, s)) out.push_back(s);
        }
    }
    static bool Place(int fd, Screen &s) {
        // "ok x y z  xx xy xz  yx yy yz  zx zy zz  width height curve hand"
        const std::string g = Ask(fd, "get " + std::to_string(s.index));
        double v[15];
        if (std::sscanf(g.c_str(), "ok %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf %lf", &v[0], &v[1], &v[2],
                        &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12], &v[13], &v[14]) != 15)
            return false;
        s.c = {v[0], v[1], v[2]};
        s.b = {{v[3], v[4], v[5]}, {v[6], v[7], v[8]}, {v[9], v[10], v[11]}};
        s.metres = v[12], s.height = v[13], s.curve = v[14];
        return true;
    }

    std::thread thread_;
    std::atomic<bool> running_{true};
    std::mutex lock_;
    std::vector<Screen> screens_;
};

// Where a ray meets a screen: distance along it, and the pixel. Rays that miss still count,
// up to 40% of the screen past an edge (`inside` says whether it's on the screen itself):
// the raw gaze can be 8 degrees or more off near the top and bottom of your view, and a
// calibration dot near an edge must still get its samples.
bool HitScreen(const Screen &s, Vec3 from, Vec3 d, double &along, double &px, double &py, bool *inside = nullptr) {
    const Vec3 p = ToBasis(s.b, from - s.c), q = ToBasis(s.b, d);
    double u, v;
    if (s.curve <= 0) {
        if (q.z >= -1e-6) return false;
        along = -p.z / q.z;
        u = p.x + q.x * along, v = p.y + q.y * along;
    } else {
        // Cylinder around the vertical line x = 0, z = r (in front of the screen).
        const double r = s.curve, pz = p.z - r;
        const double A = q.x * q.x + q.z * q.z, B = 2 * (p.x * q.x + pz * q.z), C = p.x * p.x + pz * pz - r * r;
        const double disc = B * B - 4 * A * C;
        if (A < 1e-12 || disc < 0) return false;
        along = (-B + std::sqrt(disc)) / (2 * A);  // the far wall, seen from inside
        const double x = p.x + q.x * along, z = p.z + q.z * along;
        if (r - z <= 0) return false;  // the back half of the cylinder
        u = std::atan2(x, r - z) * r;
        v = p.y + q.y * along;
    }
    if (along <= 0.05) return false;
    px = (u / s.metres + 0.5) * s.wpx;
    py = (0.5 - v / s.height) * s.hpx;
    if (inside) *inside = px >= 0 && px < s.wpx && py >= 0 && py < s.hpx;
    return px > -0.4 * s.wpx && px < 1.4 * s.wpx && py > -0.4 * s.hpx && py < 1.4 * s.hpx;
}

// Head-relative angles of a head-space direction, in degrees (see md::Direction).
void Angles(Vec3 dHead, double &yaw, double &pitch) {
    yaw = std::atan2(-dHead.x, -dHead.z) * 180 / M_PI;
    pitch = std::asin(std::clamp(dHead.y, -1.0, 1.0)) * 180 / M_PI;
}

// HIT for a head-relative direction (yaw, pitch), with the head at `head`.
std::string HitJson(const std::vector<Screen> &screens, const vr::HmdMatrix34_t &head, double yaw, double pitch) {
    const Vec3 o = Position(head);
    const Screen *best = nullptr;
    double bestAlong = 1e9, x = 0, y = 0;
    bool bestInside = false;
    const Vec3 d = Rotate(head, Direction(yaw, pitch));
    for (const auto &s : screens) {
        // A screen the ray is on beats one it only passes near; then the nearest.
        double along, px, py;
        bool inside = false;
        if (!HitScreen(s, o, d, along, px, py, &inside)) continue;
        if (!best || (inside && !bestInside) || (inside == bestInside && along < bestAlong))
            best = &s, bestAlong = along, x = px, y = py, bestInside = inside;
    }
    if (!best) return "null";
    // Pixels per degree, from rays a quarter degree off in each direction.
    constexpr double kStep = 0.25;
    double j[4] = {0, 0, 0, 0}, along, px, py;
    if (HitScreen(*best, o, Rotate(head, Direction(yaw + kStep, pitch)), along, px, py))
        j[0] = (px - x) / kStep, j[1] = (py - y) / kStep;
    if (HitScreen(*best, o, Rotate(head, Direction(yaw, pitch + kStep)), along, px, py))
        j[2] = (px - x) / kStep, j[3] = (py - y) / kStep;
    const double pxPerDeg = std::sqrt(std::fabs(j[0] * j[3] - j[1] * j[2]));
    char buf[256];
    std::snprintf(buf, sizeof buf, "{\"s\":%d,\"x\":%.2f,\"y\":%.2f,\"j\":[%.3f,%.3f,%.3f,%.3f],\"dpp\":%.5f}",
                  best->index, x, y, j[0], j[1], j[2], j[3], pxPerDeg > 1e-6 ? 1 / pxPerDeg : 0.0);
    return buf;
}

std::string SrcJson(const std::vector<Screen> &screens, const vr::HmdMatrix34_t &head, Vec3 dHead,
                    const std::string &extra = "") {
    double yaw, pitch;
    Angles(Normalize(dHead), yaw, pitch);
    char buf[96];
    std::snprintf(buf, sizeof buf, "{\"hy\":%.4f,\"hp\":%.4f,", yaw, pitch);
    return buf + extra + "\"hit\":" + HitJson(screens, head, yaw, pitch) + "}";
}

// Sources (--sources, "sources LIST" on stdin): a bit each.
enum : unsigned { kAction = 1, kMmap1 = 2, kMmap2 = 4, kLeft = 8, kRight = 16, kOwn = 32, kEye = 64, kAll = 127 };

bool ParseSources(const std::string &list, unsigned &mask) {
    static const struct {
        const char *name;
        unsigned bit;
    } names[] = {{"action", kAction}, {"mmap1", kMmap1}, {"mmap2", kMmap2}, {"left", kLeft},
                 {"right", kRight},   {"own", kOwn},     {"eye", kEye},     {"all", kAll}};
    unsigned m = 0;
    size_t at = 0;
    while (at <= list.size()) {
        const size_t comma = std::min(list.find(',', at), list.size());
        const std::string name = list.substr(at, comma - at);
        bool known = false;
        for (const auto &n : names)
            if (name == n.name) m |= n.bit, known = true;
        if (!known) return false;
        at = comma + 1;
    }
    mask = m;
    return true;
}

// Head poses of the last half second, so a sample can use the pose at its own time.
class PoseHistory {
public:
    void Add(double t, const vr::HmdMatrix34_t &m) {
        poses_.push_back({t, m});
        while (poses_.size() > 2 && t - poses_.front().t > 0.5) poses_.pop_front();
    }
    bool At(double t, vr::HmdMatrix34_t &out) const {
        if (poses_.empty()) return false;
        const Entry *best = &poses_.back();
        for (const auto &e : poses_)
            if (std::fabs(e.t - t) < std::fabs(best->t - t)) best = &e;
        out = best->m;
        return true;
    }

private:
    struct Entry {
        double t;
        vr::HmdMatrix34_t m;
    };
    std::deque<Entry> poses_;
};

std::string ExeDir() {
    char buf[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    return p.substr(0, p.rfind('/'));
}

}  // namespace

int main(int argc, char **argv) {
    bool verbose = false, watchStdin = false;
    std::atomic<unsigned> sources{kAll};
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "-v") == 0) verbose = true;
        if (std::strcmp(argv[i], "--watch-stdin") == 0) watchStdin = true;
        if (std::strcmp(argv[i], "--sources") == 0 && i + 1 < argc) {
            unsigned m;
            if (ParseSources(argv[++i], m))
                sources = m;
            else
                std::fprintf(stderr, "ft-gaze: --sources %s: unknown source (all used)\n", argv[i]);
        }
    }
    // Nice 5, before any thread starts (they inherit it): we run in the dev container's podman
    // scope, beside vrcompositor and vrserver at nice 0, and the gaze service's unit doesn't
    // reach us. Not SCHED_BATCH, as ft-eyes is: that would let each wakeup wait out another
    // task's turn, and each sample goes on to the pointer.
    errno = 0;
    const int nice0 = getpriority(PRIO_PROCESS, 0);
    if (errno == 0 && nice0 < 5 && setpriority(PRIO_PROCESS, 0, 5) != 0)
        std::fprintf(stderr, "ft-gaze: nice: %s\n", std::strerror(errno));
    // --watch-stdin: quit when stdin closes. The probe runs us through distrobox, which
    // passes neither its signals nor a closed stdout on to us, but does pass stdin's end.
    // Lines on stdin until then: "sources LIST".
    std::atomic<bool> stdinClosed{false};
    if (watchStdin)
        std::thread([&stdinClosed, &sources] {
            char c[256];
            std::string line;
            ssize_t n;
            while ((n = read(0, c, sizeof c)) > 0) {
                line.append(c, size_t(n));
                for (size_t nl; (nl = line.find('\n')) != std::string::npos; line.erase(0, nl + 1)) {
                    unsigned m;
                    if (line.compare(0, 8, "sources ") == 0 && ParseSources(line.substr(8, nl - 8), m)) sources = m;
                }
                if (line.size() > 4096) line.clear();  // no newline in sight: not ours
            }
            stdinClosed = true;
        }).detach();
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err == vr::VRInitError_None) {
        vr::VR_Shutdown();
        vr::VR_Init(&err, vr::VRApplication_Overlay);
    }
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "ft-gaze: SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    auto *sys = vr::VRSystem();
    auto *input = vr::VRInput();

    // The build puts the binary in gaze/build; the manifest is in gaze/actions.
    const std::string manifest = ExeDir() + "/../actions/ft_gaze_actions.json";
    char real[PATH_MAX];
    const vr::EVRInputError me = input->SetActionManifestPath(realpath(manifest.c_str(), real) ? real : manifest.c_str());
    vr::VRActionHandle_t gaze = vr::k_ulInvalidActionHandle;
    vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
    input->GetActionHandle("/actions/gaze/in/gaze", &gaze);
    input->GetActionSetHandle("/actions/gaze", &set);
    if (me == vr::VRInputError_None)
        std::fprintf(stderr, "ft-gaze: action manifest %s: ok\n", manifest.c_str());
    else
        std::fprintf(stderr, "ft-gaze: action manifest %s: error %d\n", manifest.c_str(), int(me));

    EyeFile eyes;
    bool haveMmap = eyes.Open();
    std::fprintf(stderr, "ft-gaze: eye-server.mmap %s\n", haveMmap ? "open" : "not available");

    OwnFile ownFile;
    Screens screens;
    screens.Start();
    PoseHistory history;
    uint32_t lastN = 0;
    float lastMeas[8] = {};
    double lastEmit = 0;
    int actionErrors = 0;
    vr::EVRInputError lastActionError = vr::VRInputError_None;
    // During a VR game, SteamVR's gaze action is left alone. With ft-gaze reading the eyes, SteamVR
    // restarted its eye tracker every 10 s or so in a game, as if the headset came off, and each
    // restart took input focus from the game: Beat Saber paused (PR #13). Of what ft-gaze reads,
    // only the action reaches SteamVR (the mmap and our tracker are files), so gaze still works
    // over the dashboard. Games are told apart the way ft-screens does it, by the scene app.
    bool inGame = false;
    double nextGameCheck = 0;
    // The mmap's layout (EyeFile::Detect) is looked for while it isn't known and the eye
    // server writes: the counter (0x38 in both layouts) ticks once per sample, so a silent
    // server (headset off) costs nothing and logs nothing. Until a layout is known, SteamVR's
    // tracker counts as unavailable. "Not recognized" waits until no layout has fitted for
    // kUnknownAfter of writing, longer than SteamVR's tracker takes to warm up after the
    // headset goes on (about 20 s), then goes to the journal at most once a minute; ft-gazed
    // shows it on the Gaze page.
    constexpr double kUnknownAfter = 30;
    double nextLayoutCheck = 0, nextLayoutLog = 0, lastWrite = 0, missSince = 0;
    uint32_t lastCounterSeen = eyes.Counter();
    // SteamVR's eye tracker creates the mmap a second or two after SteamVR starts, so it can
    // be missing when we start: look for it again every 2 s until it's there.
    double nextOpen = NowRaw() + 2.0;

    while (true) {
        const double now = NowRaw();
        if (now >= nextGameCheck) {
            nextGameCheck = now + 0.5;
            const bool game = vr::VRApplications()->GetCurrentSceneProcessId() != 0;
            if (game != inGame)
                std::fprintf(stderr, "ft-gaze: %s\n",
                             game ? "a VR game is running: SteamVR's gaze action left alone" : "the VR game ended");
            inGame = game;
        }
        vr::TrackedDevicePose_t hp;
        sys->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, &hp, 1);
        if (hp.bPoseIsValid) history.Add(now, hp.mDeviceToAbsoluteTracking);

        if (!haveMmap && now >= nextOpen) {
            nextOpen = now + 2.0;
            if ((haveMmap = eyes.Open())) {
                std::fprintf(stderr, "ft-gaze: eye-server.mmap open\n");
                lastCounterSeen = eyes.Counter();
            }
        }

        // One line per new eye sample, or at 90 Hz without a usable mmap: none, or one whose
        // layout isn't known (yet). Our own tracker needs nothing from the mmap, so it keeps
        // going then; the mmap's sources print as {"ok":0} and "eye" as null.
        EyeSample s;
        bool fresh = false;
        const uint32_t counter = eyes.Counter();
        const bool writing = haveMmap && counter != lastCounterSeen;
        lastCounterSeen = counter;
        if (writing) {
            if (now - lastWrite > 2.0) missSince = 0;  // it was silent: start counting again
            lastWrite = now;
        }
        if (haveMmap && !eyes.known && (eyes.Detecting() || (writing && now >= nextLayoutCheck))) {
            const EyeFile::Detection d = eyes.Detect(now);
            if (d == EyeFile::kFound) {
                std::fprintf(stderr, "ft-gaze: eye-server.mmap layout: %s\n", eyes.shift ? "beta (+5)" : "stable");
            } else if (d == EyeFile::kNone) {
                nextLayoutCheck = now + 1.0;
                if (!missSince) missSince = now;
                if (now - missSince >= kUnknownAfter && now >= nextLayoutLog) {
                    nextLayoutLog = now + 60.0;
                    std::fprintf(stderr, "ft-gaze: eye-server.mmap has eye data, but its layout is not recognized: "
                                         "SteamVR's eye tracking is unavailable\n");
                }
            }
        }
        const bool mmapOk = haveMmap && eyes.known;
        if (mmapOk && ReadSample(eyes, s) && s.n != lastN) fresh = true, lastN = s.n;
        if (!mmapOk && now - lastEmit >= 1.0 / 90) fresh = true, s.t = now;

        if (fresh && hp.bPoseIsValid) {
            lastEmit = now;
            const unsigned want = sources;
            const auto list = screens.Get();
            const vr::HmdMatrix34_t &headNow = hp.mDeviceToAbsoluteTracking;
            vr::HmdMatrix34_t headThen = headNow;
            if (mmapOk) history.At(s.t, headThen);

            // SteamVR's action: a room-space origin and fixation point, turned into the head
            // frame so every source reports the same kind of angles.
            std::string action = "{\"ok\":0}";
            if (!inGame && (want & kAction)) {
                vr::VRActiveActionSet_t active{};
                active.ulActionSet = set;
                active.nPriority = vr::k_nActionSetOverlayGlobalPriorityMin;
                input->UpdateActionState(&active, sizeof active, 1);
                vr::VREyeTrackingData_t e{};
                const vr::EVRInputError ae =
                    input->GetEyeTrackingDataRelativeToNow(gaze, vr::TrackingUniverseStanding, 0, &e, sizeof e);
                if (ae == vr::VRInputError_None && e.bActive && e.bValid) {
                    const Vec3 o{e.vGazeOrigin.v[0], e.vGazeOrigin.v[1], e.vGazeOrigin.v[2]};
                    const Vec3 t{e.vGazeTarget.v[0], e.vGazeTarget.v[1], e.vGazeTarget.v[2]};
                    const Vec3 dHead = RotateInverse(headNow, Normalize(t - o));
                    char extra[96];
                    std::snprintf(extra, sizeof extra, "\"tracked\":%d,\"dist\":%.3f,", int(e.bTracked), Length(t - o));
                    action = SrcJson(list, headNow, dHead, extra);
                } else if (ae != lastActionError || (verbose && ++actionErrors % 90 == 1)) {
                    std::fprintf(stderr, "ft-gaze: action: error %d active %d valid %d\n", int(ae), int(e.bActive),
                                 int(e.bValid));
                    lastActionError = ae;
                }
            }

            std::string m1 = "{\"ok\":0}", m2 = m1, left = m1, right = m1, eye = "null";
            // Only from a known layout: the unread sample's zeros would print an "unc" of 0 (eyes seen).
            if (mmapOk) {
                // lr: the angle between the two eyes' directions. It's a fraction of a degree
                // normally; when the tracker loses one eye (or during a blink) it jumps.
                auto lr = [](Vec3 l, Vec3 r) {
                    return std::acos(std::clamp(Dot(Normalize(l), Normalize(r)), -1.0, 1.0)) * 180 / M_PI;
                };
                auto eyes = [](Vec3 l, Vec3 r) {
                    double ly, lp, ry, rp;
                    Angles(Normalize(l), ly, lp);
                    Angles(Normalize(r), ry, rp);
                    char b[96];
                    std::snprintf(b, sizeof b, "\"eyes\":[[%.4f,%.4f],[%.4f,%.4f]],", ly, lp, ry, rp);
                    return std::string(b);
                };
                auto unc = [](const float *v) {
                    char b[64];
                    std::snprintf(b, sizeof b, "\"unc\":[%.5f,%.5f],", std::max(v[0], v[2]), std::max(v[3], v[5]));
                    return std::string(b);
                };
                char extra[256];
                if (want & kMmap1) {
                    std::snprintf(extra, sizeof extra, "\"dist\":%.3f,\"open\":[%.3f,%.3f],\"lr\":%.3f,", Length(s.fix1),
                                  s.open[0], s.open[1], lr(s.left1, s.right1));
                    m1 = SrcJson(list, headThen, s.left1 + s.right1, extra + eyes(s.left1, s.right1) + unc(s.var1));
                }
                if (want & kMmap2) {
                    std::snprintf(extra, sizeof extra, "\"lr\":%.3f,", lr(s.left2, s.right2));
                    m2 = SrcJson(list, headThen, s.left2 + s.right2, extra + eyes(s.left2, s.right2) + unc(s.var2));
                }
                if (want & kLeft) left = SrcJson(list, headThen, s.left2);
                if (want & kRight) right = SrcJson(list, headThen, s.right2);
                // "new" compares with the last sample, so the last measurement is kept either way.
                const float *m = s.meas;
                const bool newL = m[0] != lastMeas[0] || m[1] != lastMeas[1];
                const bool newR = m[2] != lastMeas[2] || m[3] != lastMeas[3];
                std::memcpy(lastMeas, m, sizeof lastMeas);
                if (want & kEye) {
                    std::snprintf(extra, sizeof extra, "{\"q\":[%.3g,%.3g],\"m\":[[%.4f,%.4f],[%.4f,%.4f]],\"new\":[%d,%d]}",
                                  (m[4] + m[5]) / 2, (m[6] + m[7]) / 2, m[0], m[1], m[2], m[3], int(newL), int(newR));
                    eye = extra;
                }
            }

            // Our tracker: its own sample time picks the head pose, like the mmap's.
            std::string own = "{\"ok\":0}";
            OwnSample o;
            if ((want & kOwn) && ownFile.Read(o) && now - o.t < 0.1) {
                vr::HmdMatrix34_t headOwn = headNow;
                history.At(o.t, headOwn);
                auto pair = [](bool ok, float a, float b) {
                    char p[48];
                    if (!ok) return std::string("null");
                    std::snprintf(p, sizeof p, "[%.4f,%.4f]", a, b);
                    return std::string(p);
                };
                // Stored right eye first; reported left first, like the other sources.
                const std::string extra = "\"age\":" + std::to_string(int((now - o.t) * 1000)) +
                                          ",\"eyes\":[" + pair(o.flags & 2, o.eyes[2], o.eyes[3]) + "," +
                                          pair(o.flags & 1, o.eyes[0], o.eyes[1]) + "],\"slip\":[" +
                                          pair(o.flags & 8, o.slip[2], o.slip[3]) + "," +
                                          pair(o.flags & 4, o.slip[0], o.slip[1]) + "],";
                // Where each eye's own gaze lands (left, right), for drawing them apart.
                auto eyeHit = [&](bool ok, float y, float p) {
                    return ok ? HitJson(list, headOwn, y, p) : std::string("null");
                };
                const std::string hits = "\"ehit\":[" + eyeHit(o.flags & 2, o.eyes[2], o.eyes[3]) + "," +
                                         eyeHit(o.flags & 1, o.eyes[0], o.eyes[1]) + "],";
                own = SrcJson(list, headOwn, Direction(o.yaw, o.pitch), extra + hits);
            }

            double yaw, pitch;
            const Vec3 f = Rotate(headNow, {0, 0, -1});
            yaw = std::atan2(-f.x, -f.z) * 180 / M_PI;
            pitch = std::asin(std::clamp(f.y, -1.0, 1.0)) * 180 / M_PI;
            std::printf("{\"t\":%.5f,\"age\":%.1f,\"n\":%u,\"head\":{\"yaw\":%.4f,\"pitch\":%.4f,\"hit\":%s},"
                        "\"src\":{\"action\":%s,\"mmap1\":%s,\"mmap2\":%s,\"left\":%s,\"right\":%s,\"own\":%s},"
                        "\"eye\":%s}\n",
                        s.t, (now - s.t) * 1000, s.n, yaw, pitch, HitJson(list, headNow, 0, 0).c_str(), action.c_str(),
                        m1.c_str(), m2.c_str(), left.c_str(), right.c_str(), own.c_str(), eye.c_str());
            if (std::fflush(stdout) != 0) break;  // the reader went away
        }

        vr::VREvent_t ev;
        bool quit = false;
        while (sys->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) quit = true;
        if (quit) {
            sys->AcknowledgeQuit_Exiting();
            break;
        }
        if (stdinClosed) break;
        // 250 passes a second: a new sample is printed within 4 ms (2 on average) of appearing,
        // and the pose history has a pose within 2 ms of any sample's time (a 0.2 degree head
        // turn at 100 degrees a second). Every 2 ms read the pose and the events twice as often.
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
    screens.Stop();
    vr::VR_Shutdown();
    return 0;
}
