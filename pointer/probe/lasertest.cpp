// Watch who has SteamVR's laser mouse, for the gaze-first tests (docs/gaze-first.md, tests 1-3).
// Prints the dashboard's primary device and every controller's role whenever either changes,
// and the dashboard and role events. With --snapback, when the laser goes to any device but
// ours (the ft_pointer driver's), it presses our /input/a (switchlaserhand) to take it back,
// at most every 100 ms, and prints how long that took. With --reclaim MS it also takes the
// laser back when it has been on no device (gamepad mode) for MS milliseconds.
// Runs as a background OpenVR client (in the dev container).
// Usage: lasertest [--snapback] [--reclaim MS] [--seconds N]
#include <openvr.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

static volatile std::sig_atomic_t g_stop = 0;

static void Send(const char *msg) {
    static int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    const char name[] = "ft_pointer";  // the driver's abstract socket
    std::memcpy(a.sun_path + 1, name, sizeof name - 1);
    sendto(fd, msg, std::strlen(msg), 0, reinterpret_cast<sockaddr *>(&a),
           socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sizeof name - 1));
}

static std::string Describe(vr::IVRSystem *sys, vr::TrackedDeviceIndex_t i) {
    if (i == vr::k_unTrackedDeviceIndexInvalid) return "none";
    static const char *roles[] = {"none", "left", "right", "optout", "treadmill", "stylus"};
    char type[64] = "", serial[64] = "";
    sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
    sys->GetStringTrackedDeviceProperty(i, vr::Prop_SerialNumber_String, serial, sizeof serial);
    const auto role = sys->GetControllerRoleForTrackedDeviceIndex(i);
    const int hint = sys->GetInt32TrackedDeviceProperty(i, vr::Prop_ControllerRoleHint_Int32);
    char out[200];
    std::snprintf(out, sizeof out, "%u %s %s role=%s hint=%d", i, type, serial, role < 6 ? roles[role] : "?", hint);
    return out;
}

int main(int argc, char **argv) {
    bool snapback = false;
    double seconds = 0, reclaimMs = 0;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--snapback")) snapback = true;
        else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--reclaim") && i + 1 < argc) reclaimMs = std::atof(argv[++i]);
        else {
            std::fprintf(stderr, "usage: %s [--snapback] [--reclaim MS] [--seconds N]\n", argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem *sys = vr::VR_Init(&err, vr::VRApplication_Background);
    if (err != vr::VRInitError_None) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    const auto start = Clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    std::string lastPrimary, lastRoles;
    Clock::time_point lostAt{}, pressedAt{}, noneSince{};
    bool pressed = false;
    while (!g_stop && (seconds <= 0 || ms() < seconds * 1000)) {
        vr::VREvent_t ev;
        while (sys->PollNextEvent(&ev, sizeof ev)) {
            const char *name = sys->GetEventTypeNameFromEnum(vr::EVREventType(ev.eventType));
            switch (ev.eventType) {
            case vr::VREvent_DashboardActivated:
            case vr::VREvent_DashboardDeactivated:
            case vr::VREvent_TrackedDeviceRoleChanged:
            case vr::VREvent_TrackedDeviceActivated:
            case vr::VREvent_TrackedDeviceDeactivated:
            case vr::VREvent_ButtonPress:
            case vr::VREvent_ButtonUnpress:
                std::printf("%9.1f ms  event %s device %u button %u\n", ms(), name, ev.trackedDeviceIndex,
                            ev.data.controller.button);
                break;
            default:
                break;
            }
        }
        vr::TrackedDeviceIndex_t ours = vr::k_unTrackedDeviceIndexInvalid;
        std::string roles;
        for (vr::TrackedDeviceIndex_t i = 1; i < vr::k_unMaxTrackedDeviceCount; ++i) {
            if (sys->GetTrackedDeviceClass(i) != vr::TrackedDeviceClass_Controller) continue;
            char type[64] = "";
            sys->GetStringTrackedDeviceProperty(i, vr::Prop_ControllerType_String, type, sizeof type);
            if (!std::strcmp(type, "ft_pointer")) ours = i;
            roles += "  [" + Describe(sys, i) + " connected=" + std::to_string(sys->IsTrackedDeviceConnected(i)) + "]";
        }
        if (roles != lastRoles) {
            std::printf("%9.1f ms  controllers:%s\n", ms(), roles.c_str());
            lastRoles = roles;
        }
        const auto primary = vr::VROverlay()->GetPrimaryDashboardDevice();
        const std::string p = Describe(sys, primary);
        if (p != lastPrimary) {
            std::printf("%9.1f ms  laser: %s%s\n", ms(), p.c_str(), primary == ours ? " (ours)" : "");
            if (primary == ours && lostAt != Clock::time_point{}) {
                std::printf("%9.1f ms  back on ours after %.0f ms\n", ms(),
                            std::chrono::duration<double, std::milli>(Clock::now() - lostAt).count());
                lostAt = {};
            }
            if (primary != ours && ours != vr::k_unTrackedDeviceIndexInvalid && primary != vr::k_unTrackedDeviceIndexInvalid)
                lostAt = Clock::now();
            noneSince = primary == vr::k_unTrackedDeviceIndexInvalid ? Clock::now() : Clock::time_point{};
            lastPrimary = p;
        }
        if (snapback || reclaimMs > 0) {
            const auto now = Clock::now();
            if (pressed && now - pressedAt > std::chrono::milliseconds(40)) {
                Send("btn a 0");
                pressed = false;
            }
            if (snapback && !pressed && lostAt != Clock::time_point{} && now - pressedAt > std::chrono::milliseconds(100)) {
                Send("btn a 1");
                pressed = true;
                pressedAt = now;
                std::printf("%9.1f ms  snapback: pressed our a\n", ms());
            }
            if (!pressed && reclaimMs > 0 && noneSince != Clock::time_point{} && ours != vr::k_unTrackedDeviceIndexInvalid &&
                sys->IsTrackedDeviceConnected(ours) &&
                now - noneSince > std::chrono::milliseconds(int(reclaimMs)) && now - pressedAt > std::chrono::milliseconds(100)) {
                Send("btn a 1");
                pressed = true;
                pressedAt = now;
                std::printf("%9.1f ms  reclaim: pressed our a\n", ms());
            }
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (pressed) Send("btn a 0");
    vr::VR_Shutdown();
    return 0;
}
