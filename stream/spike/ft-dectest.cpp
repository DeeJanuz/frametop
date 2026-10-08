// ft-dectest: spike S1 for remote displays (docs/remote-displays.md). Decodes an HEVC
// clip with the Frame's hardware decoder (V4L2 stateful API, /dev/video-dec0) and shows
// every decoded buffer in a SteamVR overlay through ImportDmabuf: no copy, no
// conversion pass. Above it, optionally, the source picture as RGB, so colour errors
// show side by side.
//
//   ft-dectest CLIP.h265 [--ref PATTERN.rgb] [--format q08c|nv12|ab24|qc24] [--fps 60]
//              [--seconds s] [--once] [--buffers n] [--keep n] [--distance m] [--width m]
//              [--headlocked] [--alternate s] [--convert 709|601] [--range tv|pc]
//
// CLIP is raw HEVC (Annex B), fed at --fps in real time and looped unless --once.
// PATTERN.rgb is raw RGB24 at the clip's size (pattern.py). --format picks what the
// decoder writes: NV12 in UBWC (q08c, the default) or linear (nv12), or RGBA converted by
// the decoder, linear (ab24) or in UBWC (qc24). SteamVR samples NV12 as raw Y/Cb/Cr with
// no conversion, and the RGBA formats turned out to be TP10 UBWC whatever they're called.
// --convert adds the missing pass: the GPU turns each decoded picture into RGBA with that
// matrix and --range (default tv, limited) before SteamVR gets it. --keep is how many decoded
// buffers stay with SteamVR (the one on screen and the ones before it). --headlocked
// keeps the panel fixed in view. --alternate shows the reference on the video's panel
// instead of above it, switching every s seconds and printing "show ref" / "show video",
// so s1-colours.py can grab the headset view in each state and compare them pixel by pixel.
// Every 2 s it prints frames fed and decoded, decode time, frames inside the decoder,
// the conversion's time (until the GPU is done), and this process's CPU use. Build: stream/build.sh; run in the dev container.
#include "iris.h"

namespace {

bool ReadFile(const char *path, std::vector<uint8_t> *out) {
    FILE *f = std::fopen(path, "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    out->resize(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const bool ok = std::fread(out->data(), 1, out->size(), f) == out->size();
    std::fclose(f);
    return ok;
}

// The access units (one picture each, with any parameter sets and SEI before it) of an
// Annex B HEVC stream, as [begin, end) byte ranges with their start codes.
std::vector<std::pair<size_t, size_t>> AccessUnits(const std::vector<uint8_t> &d) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 3 < d.size(); ++i)
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            starts.push_back(i > 0 && d[i - 1] == 0 ? i - 1 : i);
            i += 2;
        }
    std::vector<std::pair<size_t, size_t>> aus;
    size_t begin = 0;
    bool vcl = false;
    for (const size_t s : starts) {
        const size_t h = d[s + 2] == 0 ? s + 4 : s + 3;  // the NAL header
        if (h + 2 >= d.size()) break;
        const int type = d[h] >> 1 & 0x3f;
        const bool slice = type < 32;
        const bool first = slice && (d[h + 2] & 0x80);  // first_slice_segment_in_pic_flag
        // VPS, SPS, PPS, AUD, prefix SEI and the reserved prefix types open a new picture.
        const bool opens = (type >= 32 && type <= 35) || type == 39 || (type >= 41 && type <= 44) ||
                           (type >= 48 && type <= 55);
        if (vcl && (opens || first)) aus.emplace_back(begin, s), begin = s, vcl = false;
        vcl = vcl || slice;
    }
    if (vcl) aus.emplace_back(begin, d.size());
    return aus;
}

// The RGB reference: PATTERN.rgb in a linear ABGR8888 buffer SteamVR imports.
vr::SharedTextureHandle_t Reference(const char *path, uint32_t w, uint32_t h, gbm_bo **keep) {
    std::vector<uint8_t> rgb;
    if (!ReadFile(path, &rgb) || rgb.size() != size_t(w) * h * 3) {
        std::fprintf(stderr, "%s isn't %ux%u RGB24\n", path, w, h);
        return 0;
    }
    const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    gbm_device *gbm = drm >= 0 ? gbm_create_device(drm) : nullptr;
    gbm_bo *bo = gbm ? gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING) : nullptr;
    if (!bo) return 0;
    uint32_t pitch = 0;
    void *data = nullptr;
    auto *px = static_cast<uint8_t *>(gbm_bo_map(bo, 0, 0, w, h, GBM_BO_TRANSFER_WRITE, &pitch, &data));
    if (!px) return 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t *s = &rgb[(size_t(y) * w + x) * 3];
            uint8_t *d = px + size_t(y) * pitch + x * 4;
            d[0] = s[0], d[1] = s[1], d[2] = s[2], d[3] = 255;
        }
    gbm_bo_unmap(bo, data);
    const int fd = gbm_bo_get_fd(bo);
    const uint32_t offset = 0, stride = gbm_bo_get_stride(bo);
    *keep = bo;
    return Import(w, h, DRM_FORMAT_ABGR8888, DRM_FORMAT_MOD_LINEAR, 1, &fd, &offset, &stride);
}

}  // namespace

int main(int argc, char **argv) {
    const char *clip = nullptr, *ref = nullptr, *dev = "/dev/video-dec0";
    double fps = 60, seconds = 0, distance = 1.3, width = 1.2;
    bool once = false, headlocked = false;
    double alternate = 0;
    int buffers = 6, keep = 2;
    Decoder dec;
    Converter conv;
    bool convert = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (a == "--ref") ref = v, ++i;
        else if (a == "--format") {
            const std::map<std::string, uint32_t> formats = {
                {"q08c", V4L2_PIX_FMT_QC08C}, {"nv12", V4L2_PIX_FMT_NV12}, {"ab24", kAB24}, {"qc24", kQC24}};
            const auto f = formats.find(v);
            if (f == formats.end()) return std::fprintf(stderr, "unknown format %s\n", v), 2;
            dec.fourcc = f->second, ++i;
        }
        else if (a == "--fps") fps = std::atof(v), ++i;
        else if (a == "--seconds") seconds = std::atof(v), ++i;
        else if (a == "--buffers") buffers = std::atoi(v), ++i;
        else if (a == "--keep") keep = std::max(1, std::atoi(v)), ++i;
        else if (a == "--distance") distance = std::atof(v), ++i;
        else if (a == "--width") width = std::atof(v), ++i;
        else if (a == "--device") dev = v, ++i;
        else if (a == "--once") once = true;
        else if (a == "--headlocked") headlocked = true;
        else if (a == "--alternate") alternate = std::atof(v), ++i;
        else if (a == "--convert") convert = true, conv.matrix = std::string(v) == "601" ? EGL_ITU_REC601_EXT : EGL_ITU_REC709_EXT, ++i;
        else if (a == "--range") conv.range = std::string(v) == "pc" ? EGL_YUV_FULL_RANGE_EXT : EGL_YUV_NARROW_RANGE_EXT, ++i;
        else if (a[0] != '-') clip = argv[i];
        else return std::fprintf(stderr, "unknown option %s\n", argv[i]), 2;
    }
    if (!clip) return std::fprintf(stderr, "usage: ft-dectest CLIP.h265 [--ref PATTERN.rgb] [--format q08c|nv12|ab24|qc24] ...\n"), 2;
    dec.keep = keep;
    dec.toSteamVR = !convert;
    std::vector<uint8_t> stream;
    if (!ReadFile(clip, &stream)) return std::perror(clip), 1;
    const auto aus = AccessUnits(stream);
    if (aus.empty()) return std::fprintf(stderr, "%s: no pictures\n", clip), 1;
    std::printf("%s: %zu pictures, %.1f s at %.0f fps\n", clip, aus.size(), aus.size() / fps, fps);
    signal(SIGINT, Stop);
    signal(SIGTERM, Stop);

    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Overlay);
    if (err != vr::VRInitError_None) return std::fprintf(stderr, "openvr: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err)), 1;
    if (!dec.Open(dev, buffers)) return 1;
    if (convert && !conv.Init()) return 1;

    vr::VROverlayHandle_t video = vr::k_ulOverlayHandleInvalid, refOv = vr::k_ulOverlayHandleInvalid;
    if (vr::VROverlay()->CreateOverlay("frametop.dectest", "Decoder test", &video) != vr::VROverlayError_None)
        return std::fprintf(stderr, "can't create the overlay (already running?)\n"), 1;
    vr::VROverlay()->SetOverlayWidthInMeters(video, float(width));
    vr::VROverlay()->SetOverlayFlag(video, vr::VROverlayFlags_IgnoreTextureAlpha, true);
    vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
    vr::VRSystem()->GetDeviceToAbsoluteTrackingPose(vr::TrackingUniverseStanding, 0, poses, vr::k_unMaxTrackedDeviceCount);
    const vr::HmdMatrix34_t head = poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking;
    vr::HmdMatrix34_t pose = Ahead(head, distance, 0);
    if (headlocked) {
        vr::HmdMatrix34_t m{};
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
        m.m[2][3] = float(-distance);
        vr::VROverlay()->SetOverlayTransformTrackedDeviceRelative(video, vr::k_unTrackedDeviceIndex_Hmd, &m);
    } else {
        vr::VROverlay()->SetOverlayTransformAbsolute(video, vr::TrackingUniverseStanding, &pose);
    }

    const int64_t period = int64_t(1e9 / fps), start = MonoNs();
    int64_t next = start, lastReport = start, cpu0 = CpuNs();
    size_t au = 0;
    uint64_t fed = 0, decoded = 0;
    int intervalFed = 0, intervalDecoded = 0, stalls = 0, errWakes = 0;
    double latSum = 0, latMax = 0;
    std::map<uint64_t, int64_t> queuedAt;
    vr::SharedTextureHandle_t shownTex = 0;
    vr::Texture_t tex = {&shownTex, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
    gbm_bo *refBo = nullptr;
    vr::SharedTextureHandle_t refTex = 0;
    bool ended = false, visible = false, showingRef = false;
    int64_t nextSwitch = 0;

    while (!g_stop) {
        const int64_t now = MonoNs();
        if (seconds > 0 && now - start > int64_t(seconds * 1e9)) break;
        if (now >= next && !ended) {
            if (au == aus.size()) {
                if (once) dec.Drain(), ended = true;
                else au = 0;
            }
            if (!ended) {
                const auto [b, e] = aus[au];
                if (dec.Feed(&stream[b], e - b, fed)) {
                    queuedAt[fed] = MonoNs();
                    ++fed, ++au, ++intervalFed;
                    next += period;
                    if (MonoNs() - next > 4 * period) next = MonoNs();  // fell behind: don't burst
                } else {
                    ++stalls;
                }
            }
        }
        pollfd p = {dec.fd, POLLIN | POLLPRI | POLLOUT, 0};
        const int64_t wait = std::max<int64_t>(0, next - MonoNs());
        const int r = poll(&p, 1, int(std::min<int64_t>(wait / 1'000'000 + 1, 50)));
        if (r > 0 && (p.revents & POLLERR)) {
            ++errWakes;
            usleep(1000);
        }
        const bool configured = dec.Events();
        if (configured && convert && !conv.Setup(dec)) break;
        if (configured && ref) {
            refTex = Reference(ref, dec.width, dec.height, &refBo);
            if (refTex && alternate > 0) {
                nextSwitch = MonoNs() + int64_t(alternate * 1e9);
            } else if (refTex && vr::VROverlay()->CreateOverlay("frametop.dectest.ref", "Decoder test reference", &refOv) ==
                              vr::VROverlayError_None) {
                vr::VROverlay()->SetOverlayWidthInMeters(refOv, float(width));
                vr::VROverlay()->SetOverlayFlag(refOv, vr::VROverlayFlags_IgnoreTextureAlpha, true);
                const double h = width * dec.height / dec.width;
                vr::HmdMatrix34_t up = Ahead(head, distance, h + 0.02);
                vr::VROverlay()->SetOverlayTransformAbsolute(refOv, vr::TrackingUniverseStanding, &up);
                vr::Texture_t rt = {&refTex, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
                vr::VROverlay()->SetOverlayTexture(refOv, &rt);
                vr::VROverlay()->ShowOverlay(refOv);
            }
        }
        dec.Reclaim();
        uint64_t frame = 0;
        bool last = false;
        int i;
        while ((i = dec.Decoded(&frame, &last)) >= 0) {
            const int64_t t = MonoNs();
            auto q = queuedAt.find(frame);
            if (q != queuedAt.end()) {
                const double ms = (t - q->second) / 1e6;
                latSum += ms, latMax = std::max(latMax, ms);
                queuedAt.erase(queuedAt.begin(), std::next(q));
            }
            if (!decoded) std::printf("first picture after %llu fed\n", (unsigned long long)fed);
            ++decoded, ++intervalDecoded;
            if (convert) {
                shownTex = conv.Convert(i);
                dec.Requeue(i);  // the ring holds the picture now
            } else {
                shownTex = dec.cap[size_t(i)].tex;
            }
            if (!showingRef) vr::VROverlay()->SetOverlayTexture(video, &tex);
            if (!visible) vr::VROverlay()->ShowOverlay(video), visible = true;
            if (!convert) dec.Shown(i);
            if (last) ended = true;
        }
        if (nextSwitch && MonoNs() >= nextSwitch) {
            showingRef = !showingRef;
            nextSwitch += int64_t(alternate * 1e9);
            if (showingRef) {
                vr::Texture_t rt = {&refTex, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
                vr::VROverlay()->SetOverlayTexture(video, &rt);
            } else if (shownTex) {
                vr::VROverlay()->SetOverlayTexture(video, &tex);
            }
            std::printf("show %s\n", showingRef ? "ref" : "video");
            std::fflush(stdout);
        }
        if (ended && once && (dec.drained || last)) {
            std::printf("end of clip\n");
            sleep(2);
            break;
        }
        if (now - lastReport >= 2'000'000'000) {
            const double secs = (now - lastReport) / 1e9, cpu = (CpuNs() - cpu0) / 1e9;
            char convText[64] = "";
            if (convert)
                std::snprintf(convText, sizeof convText, "convert %.2f ms avg %.2f ms worst, ",
                              intervalDecoded ? conv.msSum / intervalDecoded : 0.0, conv.msMax);
            std::printf("%3.0f s: fed %4.1f fps, decoded %4.1f fps, decode %.1f ms avg %.1f ms worst, %llu inside, "
                        "%sCPU %.1f%% of a core, %d stalls, %d error wakes\n",
                        (now - start) / 1e9, intervalFed / secs, intervalDecoded / secs,
                        intervalDecoded ? latSum / intervalDecoded : 0.0, latMax,
                        (unsigned long long)(fed - decoded), convText, 100 * cpu / secs, stalls, errWakes);
            conv.msSum = conv.msMax = 0;
            std::fflush(stdout);
            lastReport = now, cpu0 = CpuNs();
            intervalFed = intervalDecoded = stalls = errWakes = 0, latSum = latMax = 0;
        }
    }
    if (video != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(video);
    if (refOv != vr::k_ulOverlayHandleInvalid) vr::VROverlay()->DestroyOverlay(refOv);
    if (refTex) vr::VRIPCResourceManager()->UnrefResource(refTex);
    if (refBo) gbm_bo_destroy(refBo);
    dec.Close();
    vr::VR_Shutdown();
    return 0;
}
