// ft-handtest: try the hand cutouts without restarting the desktop. Shows a test panel
// (a light grid) in front of you as its own overlay; where ft-hands tracks your hands
// in front of it, each eye sees through it, like ft-screens' screens with cutouts.
//
//   ft-handtest [--distance m] [--width m] [--seconds s]
//
// Needs hand tracking running (hands/run.sh; ft-hands publishes /run/user/UID/frametop-hands/hands).
// Build: screens/build.sh (build/ft-handtest), run in the dev container.
#include "handcut.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <signal.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

namespace {

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

int64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

vr::SharedTextureHandle_t Import(const ft_dmabuf &b) {
    vr::DmabufAttributes_t a{};
    a.unWidth = uint32_t(b.width);
    a.unHeight = uint32_t(b.height);
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = b.format;
    a.ulModifier = b.modifier;
    a.unPlaneCount = uint32_t(b.n_planes);
    for (int i = 0; i < b.n_planes && i < int(vr::MaxDmabufPlaneCount); ++i) {
        a.plane[i].unOffset = b.offset[i];
        a.plane[i].unStride = b.stride[i];
        a.plane[i].nFd = b.fd[i];
    }
    vr::SharedTextureHandle_t h = 0;
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &h)) return 0;
    return h;
}

// The stand-in "client" buffer: a light grey grid, linear so the CPU can draw it.
bool TestPattern(int drm, int w, int h, gbm_bo **out, ft_dmabuf *b) {
    gbm_device *gbm = gbm_create_device(drm);
    gbm_bo *bo = gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
    if (!bo) return false;
    uint32_t stride = 0;
    void *data = nullptr;
    auto *px = static_cast<uint8_t *>(gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &stride, &data));
    if (!px) return false;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = px + size_t(y) * stride + size_t(x) * 4;
            const bool line = x % 80 < 3 || y % 80 < 3;
            const uint8_t g = line ? 90 : uint8_t(200 + 30 * x / w);
            p[0] = g, p[1] = g, p[2] = uint8_t(line ? 160 : g), p[3] = 255;
        }
    gbm_bo_unmap(bo, data);
    *b = {};
    b->width = w, b->height = h, b->format = DRM_FORMAT_ABGR8888, b->modifier = DRM_FORMAT_MOD_LINEAR;
    b->n_planes = 1;
    b->fd[0] = gbm_bo_get_fd(bo);
    b->stride[0] = gbm_bo_get_stride(bo);
    *out = bo;
    return true;
}

}  // namespace

int main(int argc, char **argv) {
    double distance = 0.8, width = 1.0, seconds = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--distance")) distance = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--width")) width = std::atof(argv[i + 1]);
        else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(argv[i + 1]);
    }
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);

    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    uint64_t mods[64];
    uint32_t nmods = 64;
    if (!vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, DRM_FORMAT_ABGR8888, &nmods, mods))
        nmods = 0;
    std::printf("SteamVR takes %u modifiers for ABGR8888\n", nmods);

    std::map<const void *, vr::SharedTextureHandle_t> imports;
    handcut::Renderer renderer;
    if (!renderer.Init(std::vector<uint64_t>(mods, mods + nmods), [&](const handcut::Output *o) {
            auto it = imports.find(o);
            if (it != imports.end()) vr::VRIPCResourceManager()->UnrefResource(it->second), imports.erase(it);
        }))
        return 1;

    const int W = 1600, H = 900;
    const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    gbm_bo *bo = nullptr;
    ft_dmabuf client;
    if (drm < 0 || !TestPattern(drm, W, H, &bo, &client)) return std::fprintf(stderr, "can't make the test pattern\n"), 1;
    const vr::SharedTextureHandle_t plain = Import(client);
    if (!plain) return std::fprintf(stderr, "SteamVR can't import the test pattern\n"), 1;

    vr::VROverlayHandle_t ov;
    if (vr::VROverlay()->CreateOverlay("frametop.handtest", "Hand cutout test", &ov) != vr::VROverlayError_None)
        return std::fprintf(stderr, "can't create the overlay (already running?)\n"), 1;
    vr::VROverlay()->SetOverlayWidthInMeters(ov, float(width));

    // In front of the head as it is now, level, facing it.
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    const auto &hm = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    const double yaw = std::atan2(hm.m[0][2], hm.m[2][2]);
    handcut::Panel panel{};
    auto &P = panel.pose;
    P.m[0][0] = float(std::cos(yaw)), P.m[0][2] = float(std::sin(yaw));
    P.m[1][1] = 1;
    P.m[2][0] = float(-std::sin(yaw)), P.m[2][2] = float(std::cos(yaw));
    P.m[0][3] = float(hm.m[0][3] - std::sin(yaw) * distance);
    P.m[1][3] = float(hm.m[1][3] - 0.15);
    P.m[2][3] = float(hm.m[2][3] - std::cos(yaw) * distance);
    panel.width = width, panel.height = width * H / W, panel.curve = 0, panel.pxWidth = W, panel.pxHeight = H;
    vr::VROverlay()->SetOverlayTransformAbsolute(ov, vr::TrackingUniverseStanding, &P);
    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    vr::SharedTextureHandle_t shown = plain;
    vr::Texture_t tex = {&shown, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    vr::VROverlay()->SetOverlayTexture(ov, &tex);
    vr::VROverlay()->ShowOverlay(ov);
    std::printf("test panel %.2f m wide, %.2f m ahead; Ctrl+C to stop\n", width, distance);

    handcut::Hands hands;
    bool cutting = false;
    const int64_t start = MonoNs();
    int64_t lastReport = start;
    int frames = 0, cutFrames = 0;
    double ms = 0, worst = 0;
    size_t caps2d = 0;
    while (!g_stop && (seconds <= 0 || (MonoNs() - start) / 1e9 < seconds)) {
        const auto tick = std::chrono::steady_clock::now();
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const auto &head = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
        const int64_t now = MonoNs();
        std::vector<handcut::Capsule2D> eyes2d[2];
        bool cut = false;
        if (hands.Update(head, now)) {
            double eyes[2][3];
            handcut::EyePositions(head, eyes);
            cut = handcut::Project(panel, hands.capsules(), eyes, eyes2d);
        }
        const handcut::Output *out = cut ? renderer.Composite(0, bo, 1, client, eyes2d) : nullptr;
        if (out) {
            auto it = imports.find(out);
            if (it == imports.end()) {
                const vr::SharedTextureHandle_t h = Import(out->buf);
                if (!h) std::fprintf(stderr, "SteamVR can't import the output buffer\n");
                it = imports.emplace(out, h).first;
            }
            if (it->second) {
                if (!cutting) {
                    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_IgnoreTextureAlpha, false);
                    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_SideBySide_Parallel, true);
                    cutting = true;
                }
                if (shown != it->second) shown = it->second, vr::VROverlay()->SetOverlayTexture(ov, &tex);
                ms += renderer.lastMs(), worst = std::max(worst, renderer.lastMs());
                ++cutFrames;
                caps2d += eyes2d[0].size() + eyes2d[1].size();
            }
        } else if (cutting) {
            vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_SideBySide_Parallel, false);
            vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_IgnoreTextureAlpha, true);
            shown = plain;
            vr::VROverlay()->SetOverlayTexture(ov, &tex);
            cutting = false;
        }
        ++frames;
        if (now - lastReport > 2'000'000'000) {
            const handcut::CutStats st = renderer.TakeStats();
            std::printf("%.0f s: %d ticks, %d with a cutout (%.1f capsules per eye), composite %.2f ms avg %.2f ms worst, "
                        "%zu hand capsules known; %d draws (%d partial), %d same, %d busy\n",
                        (now - start) / 1e9, frames, cutFrames, cutFrames ? caps2d / 2.0 / cutFrames : 0.0,
                        cutFrames ? ms / cutFrames : 0.0, worst, hands.capsules().size(), st.draws, st.partial, st.same,
                        st.busy);
            std::fflush(stdout);
            lastReport = now, frames = cutFrames = 0, ms = worst = 0, caps2d = 0;
        }
        std::this_thread::sleep_until(tick + std::chrono::microseconds(11111));
    }
    vr::VROverlay()->DestroyOverlay(ov);
    for (auto &[k, h] : imports)
        if (h) vr::VRIPCResourceManager()->UnrefResource(h);
    imports.clear();
    vr::VRIPCResourceManager()->UnrefResource(plain);
    vr::VR_Shutdown();
    return 0;
}
