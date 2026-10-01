// Does a dashboard overlay with overlay flag 1 << 4 take SteamVR's input focus for its own process
// (docs/gaze-first.md)? SteamVR's dashboard gives an external overlay's page a "VR client" input
// focus (its owner's PID) when that flag is set, and a Steam Input one with 1 << 30. While a VR
// client has the input focus, as a game does, Steam shouldn't get the controllers.
// Shows a dashboard overlay "Frametop focus test" (a plain colour) with that flag, and prints
// input-focus events, IsInputAvailable(), and the dashboard's visibility as they change.
// Runs as an OpenVR overlay client (in the dev container).
// With --manifest it's a SteamVR input client too (focustest_actions/: Y and the triggers), and
// prints when those actions change, which shows whether it gets them while Steam doesn't.
// Usage: focustest [--seconds N] [--no-flag] [--steam-flag] [--manifest]
#include <openvr.h>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>
#include <stdlib.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using Clock = std::chrono::steady_clock;
static volatile std::sig_atomic_t g_stop = 0;

int main(int argc, char **argv) {
    double seconds = 0;
    bool flag = true, steamFlag = false, manifest = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--no-flag")) flag = false;
        else if (!std::strcmp(argv[i], "--steam-flag")) steamFlag = true;
        else if (!std::strcmp(argv[i], "--manifest")) manifest = true;
        else {
            std::fprintf(stderr, "usage: %s [--seconds N] [--no-flag] [--steam-flag] [--manifest]\n", argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    vr::EVRInitError err = vr::VRInitError_None;
    vr::IVRSystem *sys = vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) {
        std::printf("VR_Init failed: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::IVROverlay *ov = vr::VROverlay();
    vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
    vr::VRActionHandle_t actY = vr::k_ulInvalidActionHandle, actTrigger = vr::k_ulInvalidActionHandle;
    if (manifest) {
        char exe[PATH_MAX] = "";
        const ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        std::string dir = n > 0 ? std::string(exe, n) : std::string(".");
        dir = dir.substr(0, dir.rfind('/'));  // build/
        dir = dir.substr(0, dir.rfind('/'));  // pointer/probe
        const std::string path = dir + "/focustest_actions/actions.json";
        const auto ie = vr::VRInput()->SetActionManifestPath(path.c_str());
        vr::VRInput()->GetActionSetHandle("/actions/focus", &set);
        vr::VRInput()->GetActionHandle("/actions/focus/in/y", &actY);
        vr::VRInput()->GetActionHandle("/actions/focus/in/trigger", &actTrigger);
        std::printf("action manifest %s: error %d\n", path.c_str(), int(ie));
    }
    vr::VROverlayHandle_t main = vr::k_ulOverlayHandleInvalid, thumb = vr::k_ulOverlayHandleInvalid;
    auto e = ov->CreateDashboardOverlay("frametop.focustest", "Frametop focus test", &main, &thumb);
    if (e != vr::VROverlayError_None) {
        std::printf("CreateDashboardOverlay: %s\n", ov->GetOverlayErrorNameFromEnum(e));
        return 1;
    }
    // A plain teal page, and a teal thumbnail.
    const uint32_t w = 256, h = 160;
    std::vector<uint8_t> px(w * h * 4);
    for (uint32_t i = 0; i < w * h; ++i) px[i * 4] = 20, px[i * 4 + 1] = 140, px[i * 4 + 2] = 140, px[i * 4 + 3] = 255;
    ov->SetOverlayRaw(main, px.data(), w, h, 4);
    ov->SetOverlayRaw(thumb, px.data(), w, h, 4);
    ov->SetOverlayWidthInMeters(main, 1.5f);
    ov->SetOverlayInputMethod(main, vr::VROverlayInputMethod_Mouse);
    if (flag) {
        e = ov->SetOverlayFlag(main, vr::VROverlayFlags(1 << 4), true);
        std::printf("SetOverlayFlag(1 << 4): %s\n", ov->GetOverlayErrorNameFromEnum(e));
    }
    if (steamFlag) {
        e = ov->SetOverlayFlag(main, vr::VROverlayFlags(1 << 30), true);
        std::printf("SetOverlayFlag(1 << 30): %s\n", ov->GetOverlayErrorNameFromEnum(e));
    }
    uint32_t flags = 0;
    ov->GetOverlayFlags(main, &flags);
    std::printf("overlay flags now 0x%x; pid %d\n", flags, int(getpid()));
    std::fflush(stdout);

    const auto start = Clock::now();
    auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - start).count(); };
    int lastAvail = -1, lastDash = -1, lastActive = -1, lastY = -1, lastTrigger = -1;
    while (!g_stop && (seconds <= 0 || ms() < seconds * 1000)) {
        vr::VREvent_t ev;
        while (sys->PollNextEvent(&ev, sizeof ev)) {
            const char *name = sys->GetEventTypeNameFromEnum(vr::EVREventType(ev.eventType));
            if (ev.eventType == vr::VREvent_InputFocusChanged || ev.eventType == vr::VREvent_InputFocusCaptured ||
                ev.eventType == vr::VREvent_InputFocusReleased)
                std::printf("%9.1f ms  %s pid %u (old %u)\n", ms(), name, ev.data.process.pid, ev.data.process.oldPid);
            else if (ev.eventType == vr::VREvent_ButtonPress || ev.eventType == vr::VREvent_ButtonUnpress ||
                     ev.eventType == vr::VREvent_DashboardActivated || ev.eventType == vr::VREvent_DashboardDeactivated)
                std::printf("%9.1f ms  %s device %u button %u\n", ms(), name, ev.trackedDeviceIndex, ev.data.controller.button);
        }
        while (ov->PollNextOverlayEvent(main, &ev, sizeof ev)) {
            const char *name = sys->GetEventTypeNameFromEnum(vr::EVREventType(ev.eventType));
            if (ev.eventType != vr::VREvent_MouseMove && std::strncmp(name, "Unknown", 7) != 0 &&
                std::strcmp(name, "VREvent_OverlayMouseFocusChanged") != 0 && ev.eventType != vr::VREvent_PropertyChanged)
                std::printf("%9.1f ms  overlay: %s\n", ms(), name);
        }
        if (manifest) {
            vr::VRActiveActionSet_t active{};
            active.ulActionSet = set;
            vr::VRInput()->UpdateActionState(&active, sizeof active, 1);
            vr::InputDigitalActionData_t y{}, t{};
            vr::VRInput()->GetDigitalActionData(actY, &y, sizeof y, vr::k_ulInvalidInputValueHandle);
            vr::VRInput()->GetDigitalActionData(actTrigger, &t, sizeof t, vr::k_ulInvalidInputValueHandle);
            const int yv = y.bActive ? y.bState : -2, tv = t.bActive ? t.bState : -2;
            if (yv != lastY || tv != lastTrigger) {
                std::printf("%9.1f ms  our actions: y %d, trigger %d (-2: inactive)\n", ms(), yv, tv);
                lastY = yv, lastTrigger = tv;
            }
        }
        const int avail = sys->IsInputAvailable(), dash = ov->IsDashboardVisible(), active = ov->IsActiveDashboardOverlay(main);
        if (avail != lastAvail || dash != lastDash || active != lastActive) {
            std::printf("%9.1f ms  input available %d, dashboard %d, our page active %d\n", ms(), avail, dash, active);
            lastAvail = avail, lastDash = dash, lastActive = active;
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    vr::VR_Shutdown();
    return 0;
}
