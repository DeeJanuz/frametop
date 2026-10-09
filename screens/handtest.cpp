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
#include <string>
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

// --probe: where the cutouts would land, against the hand Room View shows. The panel is
// see-through, with dots on the wrist, middle knuckle and fingertips of each tracked hand,
// one colour per timing: magenta where the cameras saw the hand, cyan moved ahead to now,
// green moved ahead to now + lead (what the cutouts use). White dots mark the panel's
// corners. Record the headset view meanwhile and compare (frame-hands/probes/
// probe_video.py), or look: which colour sits on your fingertips, still and moving?
// Each tick goes to the log as a JSON line, after a header line with the panel and eyes.
struct Variant {
    const char *name;
    bool predict;
    double leadMs;
    float rgb[3];
};
const int kProbePoints[] = {0, 9, 4, 8, 12, 16, 20};

void Matrix(FILE *f, const handcut::Mat &m) {
    std::fprintf(f, "[");
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) std::fprintf(f, "%s%.6f", r || c ? "," : "", m.m[r][c]);
    std::fprintf(f, "]");
}

void Point(FILE *f, const float p[3]) { std::fprintf(f, "[%.5f,%.5f,%.5f]", p[0], p[1], p[2]); }

int RunProbe(vr::VROverlayHandle_t ov, const handcut::Panel &panel, handcut::Renderer &renderer,
             std::map<const void *, vr::SharedTextureHandle_t> &imports, double seconds, double leadMs, double dotMm,
             const char *logPath) {
    handcut::Hands hands;
    if (leadMs < 0) leadMs = hands.leadMs();
    const Variant variants[] = {{"seen", false, 0, {1, 0, 1}}, {"now", true, 0, {0, 1, 1}}, {"lead", true, leadMs, {0, 1, 0}}};
    FILE *log = std::fopen(logPath, "w");
    if (!log) return std::perror(logPath), 1;
    std::fprintf(log, "{\"probe\":1,\"panel\":{\"pose\":");
    Matrix(log, panel.pose);
    std::fprintf(log, ",\"width\":%.4f,\"height\":%.4f,\"px\":[%d,%d]},\"eyes\":[", panel.width, panel.height,
                 panel.pxWidth, panel.pxHeight);
    for (vr::EVREye e : {vr::Eye_Left, vr::Eye_Right}) {
        Matrix(log, vr::VRSystem()->GetEyeToHeadTransform(e));
        std::fprintf(log, e == vr::Eye_Left ? "," : "],");
    }
    std::fprintf(log, "\"points\":[0,9,4,8,12,16,20],\"dot_mm\":%.2f,\"variants\":[", dotMm);
    for (size_t k = 0; k < 3; ++k)
        std::fprintf(log, "%s{\"name\":\"%s\",\"predict\":%s,\"lead_ms\":%.1f,\"rgb\":[%.0f,%.0f,%.0f]}", k ? "," : "",
                     variants[k].name, variants[k].predict ? "true" : "false", variants[k].leadMs,
                     variants[k].rgb[0] * 255, variants[k].rgb[1] * 255, variants[k].rgb[2] * 255);
    std::fprintf(log, "]}\n");

    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_IgnoreTextureAlpha, false);
    vr::VROverlay()->SetOverlayFlag(ov, vr::VROverlayFlags_SideBySide_Parallel, true);
    vr::SharedTextureHandle_t shown = 0;
    vr::Texture_t tex = {&shown, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    std::printf("probe: magenta = as seen, cyan = now, green = now + %.0f ms; log %s\n", leadMs, logPath);

    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    const int64_t start = MonoNs();
    int64_t lastReport = start;
    int ticks = 0, withHands = 0;
    std::vector<handcut::Hands::HandPoints> pts;
    while (!g_stop && (seconds <= 0 || (MonoNs() - start) / 1e9 < seconds)) {
        const auto tick = std::chrono::steady_clock::now();
        vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
        const auto &head = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
        const int64_t now = MonoNs();
        hands.Update(head, now);
        double eyes[2][3];
        handcut::EyePositions(head, eyes);
        std::vector<handcut::Mark> marks[2];
        const float W = float(panel.pxWidth), H = float(panel.pxHeight);
        for (int e = 0; e < 2; ++e)
            for (float x : {14.f, W - 14}) for (float y : {14.f, H - 14}) marks[e].push_back({x, y, 6, {1, 1, 1}});
        std::fprintf(log, "{\"t\":%lld,\"cap\":%lld,\"head\":", (long long)now, (long long)hands.captureNs());
        Matrix(log, head);
        std::fprintf(log, ",\"hands\":[");
        bool any = false;
        for (size_t v = 0; v < 3; ++v) {
            hands.Points(now, variants[v].predict, variants[v].leadMs, pts);
            for (size_t h = 0; h < pts.size(); ++h) {
                std::fprintf(log, "%s{\"v\":%zu,\"id\":%u,\"right\":%d,\"p\":[", any ? "," : "", v, pts[h].id, pts[h].right);
                any = true;
                for (size_t j = 0; j < sizeof kProbePoints / sizeof *kProbePoints; ++j) {
                    const float *p = pts[h].p[kProbePoints[j]];
                    if (j) std::fputc(',', log);
                    Point(log, p);
                    const handcut::Capsule c{{p[0], p[1], p[2]}, {p[0], p[1], p[2]}, float(dotMm / 1000), float(dotMm / 1000)};
                    std::vector<handcut::Capsule2D> on[2];
                    if (!handcut::Project(panel, {c}, eyes, on)) continue;
                    for (int e = 0; e < 2; ++e)
                        for (const auto &d : on[e])
                            marks[e].push_back({d.ax, d.ay, std::max(4.f, d.ra), {variants[v].rgb[0], variants[v].rgb[1], variants[v].rgb[2]}});
                }
                std::fprintf(log, "]}");
            }
        }
        std::fprintf(log, "]}\n");
        withHands += any;
        const handcut::Output *out = renderer.Marks(0, panel.pxWidth, panel.pxHeight, marks);
        if (out) {
            auto it = imports.find(out);
            if (it == imports.end()) it = imports.emplace(out, Import(out->buf)).first;
            if (it->second && shown != it->second) {
                const bool first = !shown;
                shown = it->second;
                vr::VROverlay()->SetOverlayTexture(ov, &tex);
                if (first) vr::VROverlay()->ShowOverlay(ov);
            }
        }
        ++ticks;
        if (now - lastReport > 2'000'000'000) {
            std::printf("%.0f s: %d ticks, %d with hands\n", (now - start) / 1e9, ticks, withHands);
            std::fflush(stdout);
            std::fflush(log);
            lastReport = now, ticks = withHands = 0;
        }
        std::this_thread::sleep_until(tick + std::chrono::microseconds(11111));
    }
    std::fclose(log);
    return 0;
}

}  // namespace

int main(int argc, char **argv) {
    double distance = 0.8, width = 1.0, seconds = 0, leadMs = -1, dotMm = 3;
    bool probe = false;
    std::string logPath = "/tmp/handprobe-" + std::to_string(time(nullptr)) + ".jsonl";
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--probe")) probe = true;
        else if (!std::strcmp(argv[i], "--distance") && more) distance = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--width") && more) width = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--seconds") && more) seconds = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--lead") && more) leadMs = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--dot-mm") && more) dotMm = std::atof(argv[++i]);
        else if (!std::strcmp(argv[i], "--log") && more) logPath = argv[++i];
        else return std::fprintf(stderr, "usage: ft-handtest [--distance m] [--width m] [--seconds s] "
                                         "[--probe [--lead ms] [--dot-mm mm] [--log FILE]]\n"), 2;
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
    if (probe) {
        const int rc = RunProbe(ov, panel, renderer, imports, seconds, leadMs, dotMm, logPath.c_str());
        vr::VROverlay()->DestroyOverlay(ov);
        for (auto &[k, h] : imports)
            if (h) vr::VRIPCResourceManager()->UnrefResource(h);
        imports.clear();
        vr::VRIPCResourceManager()->UnrefResource(plain);
        vr::VR_Shutdown();
        return rc;
    }
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
