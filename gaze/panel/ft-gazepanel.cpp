// ft-gazepanel: the gaze calibration panel (docs/design.md, gaze/README.md). A SteamVR overlay
// fixed to the headset, so wherever you turn your head it stays in the same place in your
// view: a dot drawn at a head-relative direction is exactly that direction from the headset,
// which is what the gaze service needs to know where you were looking. It's drawn on the CPU
// and takes no input: the gaze service (gaze/ft-gazed) drives it, and the pointer helper
// passes it your presses (calaccept, calquit).
//
// The panel sits POINTER-like at --distance (1.5 m, about where Frametop's screens are, so
// the eyes converge as they do in use). "quick" is a small square, QUICK_DEG across, for the
// one-dot check; "five" is FIVE_DEG across (4:3), see-through like quick, for the five-dot
// check, whose dots are 12 degrees left and right and 9 up and down; "full" is FULL_DEG
// across (4:3), with a solid background whose brightness the service sets per round (pupil
// size changes with it, and the tracker's error with it); "fit" is FIT_DEG across (4:3),
// see-through like quick, for the headset fit check: a card per eye (tracked or lost, the
// tracker's signal, how much of the last 10 s it was seen) and hints. Every dot a check shows
// must fit its panel: gazecheck.py's PANEL_DEG mirrors these sizes and checks it.
//
// Control socket: abstract unix datagram "@ft_gazepanel" (--socket NAME); a sender with an
// address gets "ok" or "error ...":
//   show quick|five|full|fit     the panel, empty, in front of you
//   hide
//   bg <0..1>                    the background's brightness (full)
//   dot <yaw> <pitch> <state> [<progress 0..1>]
//                                the dot, head-relative degrees (yaw +left, pitch +up); state:
//                                look, capture (a ring filling to progress), done,
//                                fail, off
//   title <text> / text <text>   a line at the top / at the bottom (empty to clear)
//   note <text>                  a warning line just above the bottom one, in orange (red on
//                                the bright round): why a dot wasn't taken (empty to clear)
//   eye <0|1> <r> <g> <b> <signal 0..1|-1> <seen 0..1|-1> <word>
//                                fit: an eye's card (0 left): its state in that colour, the
//                                tracker's signal, the share of the last 10 s it was seen
//   hints <line>|<line>|...      fit: lines under the cards (empty to clear)
//   ping
//
// Each picture goes into the next of three shared buffers (linear DMA-BUFs SteamVR imported
// once, the size of the biggest panel; the texture bounds show the part in use), and the panel
// switches to it, as screens/keyboard.cpp does. SetOverlayRaw, which uploads a new texture each
// time, flickered on every change of the full calibration's 1024x768 picture, and in a live
// test the headset kept showing an old picture after the panel had drawn new ones (2026-10-01).
// It's only the fallback. A "show" makes the panel visible once its first picture is in.
//
// Options: --watch-stdin (quit when stdin closes: the service runs it), --socket NAME,
// --distance METRES. Runs in the dev container (gaze/build.sh builds it into gaze/build).
#include <openvr.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kQuickDeg = 16;      // QUICK_DEG: the one-dot check's square
constexpr double kFullDeg = 64;       // FULL_DEG: the full calibration's width (4:3)
constexpr double kFitDeg = 40;        // FIT_DEG: the headset fit check's width (4:3)
constexpr double kFiveDeg = 40;       // FIVE_DEG: the five-dot check's width (4:3), past its dots
constexpr int kQuickPx = 320, kFullW = 1024, kFullH = 768, kFitW = 800, kFitH = 600, kFiveW = 800, kFiveH = 600;
std::atomic<bool> g_stop{false};

// ---------------------------------------------------------------- text (as screens/keyboard.cpp)

stbtt_fontinfo g_font;
std::vector<unsigned char> g_fontData;
bool g_fontOk = false;
constexpr int kMaxW = kFullW, kMaxH = kFullH;  // the buffers' size: the biggest panel
struct Glyph {
    std::vector<unsigned char> bitmap;
    int w = 0, h = 0, xoff = 0, yoff = 0, advance = 0;
};
std::map<std::pair<uint32_t, int>, Glyph> g_glyphs;

void LoadFont() {
    std::string path;
    if (FILE *p = popen("fc-match -f '%{file}' 'Noto Sans' 2>/dev/null", "r")) {
        char buf[512];
        if (std::fgets(buf, sizeof buf, p)) path = buf;
        pclose(p);
    }
    if (path.empty()) path = "/usr/share/fonts/google-noto-vf/NotoSans[wght].ttf";
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        g_fontData.resize(size_t(std::ftell(f)));
        std::fseek(f, 0, SEEK_SET);
        g_fontOk = std::fread(g_fontData.data(), 1, g_fontData.size(), f) == g_fontData.size() &&
                   stbtt_InitFont(&g_font, g_fontData.data(), stbtt_GetFontOffsetForIndex(g_fontData.data(), 0));
        std::fclose(f);
    }
    if (!g_fontOk) std::fprintf(stderr, "ft-gazepanel: no font (%s); no text\n", path.c_str());
}

const Glyph &GetGlyph(uint32_t cp, int size) {
    auto [it, fresh] = g_glyphs.try_emplace({cp, size});
    Glyph &g = it->second;
    if (fresh) {
        const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
        unsigned char *b = stbtt_GetCodepointBitmap(&g_font, 0, scale, int(cp), &g.w, &g.h, &g.xoff, &g.yoff);
        if (b) g.bitmap.assign(b, b + size_t(g.w) * g.h), stbtt_FreeBitmap(b, nullptr);
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, int(cp), &adv, &lsb);
        g.advance = int(std::lround(adv * scale));
    }
    return g;
}

std::vector<uint32_t> Codepoints(const std::string &s) {
    std::vector<uint32_t> out;
    for (const unsigned char *p = (const unsigned char *)s.c_str(); *p;) {
        uint32_t c = *p++;
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (more) c &= 0x3Fu >> more;
        for (; more && (*p & 0xC0) == 0x80; --more) c = c << 6 | (*p++ & 0x3F);
        out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------- the picture

struct EyeCard {
    std::string word = "no data";
    double r = 0.6, g = 0.6, b = 0.6, signal = -1, seen = -1;
};

struct Panel {
    bool full = false, fit = false;
    EyeCard eyes[2];
    std::vector<std::string> hints;
    int w = kQuickPx, h = kQuickPx;
    double wDeg = kQuickDeg;  // across
    std::vector<uint8_t> px;
    double bg = 0.05;
    std::string title, text, note;
    bool dotOn = false;
    double dotYaw = 0, dotPitch = 0, progress = 0;
    std::string state = "off";
};

void Blend(Panel &p, int x, int y, double r, double g, double b, double a) {
    if (x < 0 || y < 0 || x >= p.w || y >= p.h || a <= 0) return;
    uint8_t *q = &p.px[(size_t(y) * p.w + x) * 4];
    a = std::min(a, 1.0);
    q[0] = uint8_t(std::lround(q[0] + (r * 255 - q[0]) * a));
    q[1] = uint8_t(std::lround(q[1] + (g * 255 - q[1]) * a));
    q[2] = uint8_t(std::lround(q[2] + (b * 255 - q[2]) * a));
    q[3] = uint8_t(std::lround(q[3] + (255 - q[3]) * a));
}

// A filled disc, or (inner > 0) a ring from inner to outer radius, anti-aliased; with sweep < 1,
// only that share of the ring, clockwise from the top.
void Disc(Panel &p, double cx, double cy, double outer, double inner, double r, double g, double b, double a,
          double sweep = 1) {
    const int x0 = int(cx - outer - 1), x1 = int(cx + outer + 1), y0 = int(cy - outer - 1), y1 = int(cy + outer + 1);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            const double dx = x + 0.5 - cx, dy = y + 0.5 - cy, d = std::hypot(dx, dy);
            double cover = std::clamp(outer - d + 0.5, 0.0, 1.0);
            if (inner > 0) cover = std::min(cover, std::clamp(d - inner + 0.5, 0.0, 1.0));
            if (sweep < 1) {
                double ang = std::atan2(dx, -dy) / (2 * M_PI);  // 0 at the top, clockwise
                if (ang < 0) ang += 1;
                if (ang > sweep) continue;
            }
            Blend(p, x, y, r, g, b, a * cover);
        }
}

void Rect(Panel &p, int x0, int y0, int x1, int y1, double r, double g, double b, double a) {
    for (int y = std::max(y0, 0); y < std::min(y1, p.h); ++y)
        for (int x = std::max(x0, 0); x < std::min(x1, p.w); ++x) Blend(p, x, y, r, g, b, a);
}

// Text centred on (x, cy), or starting at x (left).
void Text(Panel &p, const std::string &s, int size, int x, int cy, double r, double g, double b, bool left = false) {
    if (!g_fontOk || s.empty()) return;
    const auto cps = Codepoints(s);
    int width = 0;
    for (uint32_t cp : cps) width += GetGlyph(cp, size).advance;
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    const float scale = stbtt_ScaleForPixelHeight(&g_font, float(size));
    if (!left) x -= width / 2;
    const int baseline = cy + int(std::lround((ascent + descent) * scale / 2));
    for (uint32_t cp : cps) {
        const Glyph &gl = GetGlyph(cp, size);
        for (int gy = 0; gy < gl.h; ++gy)
            for (int gx = 0; gx < gl.w; ++gx)
                Blend(p, x + gl.xoff + gx, baseline + gl.yoff + gy, r, g, b, gl.bitmap[size_t(gy) * gl.w + gx] / 255.0);
        x += gl.advance;
    }
}

void Text(Panel &p, const std::string &s, int size, int x, int cy, double lum, bool left = false) {
    Text(p, s, size, x, cy, lum, lum, lum, left);
}

// The headset fit check (see the top): a card per eye, then the hints.
void DrawFit(Panel &p, double pxPerDeg, int textSize, double faint) {
    const int margin = int(p.w * 0.06), cw = int(p.w * 0.41), ch = int(p.h * 0.32), top = int(p.h * 0.12);
    const int pad = int(pxPerDeg * 0.9), big = int(pxPerDeg * 1.6), small = int(pxPerDeg * 0.8);
    for (int k = 0; k < 2; ++k) {
        const EyeCard &e = p.eyes[k];
        const int x0 = k == 0 ? margin : p.w - margin - cw;
        Rect(p, x0, top, x0 + cw, top + ch, 1, 1, 1, 0.07);
        Text(p, k ? "Right eye" : "Left eye", textSize, x0 + pad, top + pad + textSize / 2, faint, true);
        Text(p, e.word, big, x0 + pad, top + int(ch * 0.40), e.r, e.g, e.b, true);
        // The tracker's signal: a bar, red to green.
        const int by = top + int(ch * 0.62), bh = std::max(4, int(pxPerDeg * 0.35)), bw = cw - 2 * pad;
        Text(p, "Signal", small, x0 + pad, by - small, faint, true);
        Rect(p, x0 + pad, by, x0 + pad + bw, by + bh, 1, 1, 1, 0.15);
        if (e.signal >= 0) {
            const double v = std::clamp(e.signal, 0.0, 1.0);
            const double r = v < 0.5 ? 1.0 : 1.0 - 1.3 * (v - 0.5), g = v < 0.5 ? 0.3 + 0.9 * v : 0.75 + 0.5 * (v - 0.5);
            Rect(p, x0 + pad, by, x0 + pad + int(bw * v), by + bh, r, g, 0.35, 0.95);
        }
        char seen[64] = "Seen: not yet";
        if (e.seen >= 0) std::snprintf(seen, sizeof seen, "Seen %d%% of the last 10 s", int(std::lround(e.seen * 100)));
        Text(p, seen, small, x0 + pad, top + ch - pad, faint, true);
    }
    int y = top + ch + pad * 2;
    for (const std::string &line : p.hints) {
        Text(p, line, textSize, margin, y, faint, true);
        y += int(textSize * 1.4);
    }
}

// Head-relative direction -> panel pixel: the panel is a plane `d` in front of the headset.
void ToPixel(const Panel &p, double yaw, double pitch, double &x, double &y) {
    const double yr = yaw * M_PI / 180, pr = pitch * M_PI / 180;
    const double half = std::tan(p.wDeg * M_PI / 360);  // half the width, per unit of distance
    const double X = -std::tan(yr), Y = std::tan(pr) / std::cos(yr);
    x = (X / (2 * half) + 0.5) * p.w;
    y = (0.5 - Y / (2 * half) * p.w / p.h) * p.h;
}

void Draw(Panel &p) {
    p.px.assign(size_t(p.w) * p.h * 4, 0);
    const double pxPerDeg = p.w / p.wDeg;
    if (p.full) {
        for (size_t i = 0; i < p.px.size(); i += 4)
            p.px[i] = p.px[i + 1] = p.px[i + 2] = uint8_t(std::lround(p.bg * 255)), p.px[i + 3] = 255;
    } else {
        // The quick check and the fit check: a dim rounded panel, see-through, so it's clear of what's behind.
        const double r = std::min(p.w, p.h) * 0.12;
        for (int y = 0; y < p.h; ++y)
            for (int x = 0; x < p.w; ++x) {
                const double dx = std::max({r - x - 0.5, x + 0.5 - (p.w - r), 0.0});
                const double dy = std::max({r - y - 0.5, y + 0.5 - (p.h - r), 0.0});
                const double cover = std::clamp(r - std::hypot(dx, dy) + 0.5, 0.0, 1.0);
                Blend(p, x, y, 0.06, 0.06, 0.07, 0.82 * cover);
            }
    }
    const bool light = p.full && p.bg > 0.5;  // a dark dot on the bright round
    const double ink = light ? 0.0 : 1.0, faint = light ? 0.2 : 0.75;
    const int titleSize = int(pxPerDeg * (p.full ? 1.5 : 1.1)), textSize = int(pxPerDeg * (p.full ? 1.2 : 0.9));
    Text(p, p.title, titleSize, p.w / 2, int(titleSize * 1.2), faint);
    Text(p, p.text, textSize, p.w / 2, p.h - int(textSize * 1.3), faint);
    // Under the full calibration's lowest dots (RING degrees down) and over the text.
    if (light) Text(p, p.note, textSize, p.w / 2, p.h - int(textSize * 2.8), 0.7, 0.12, 0.05);
    else Text(p, p.note, textSize, p.w / 2, p.h - int(textSize * 2.8), 1.0, 0.62, 0.3);
    if (p.fit) DrawFit(p, pxPerDeg, textSize, faint);
    if (!p.dotOn || p.state == "off") return;
    double x, y;
    ToPixel(p, p.dotYaw, p.dotPitch, x, y);
    const double core = 0.22 * pxPerDeg;
    if (p.state == "look") {
        // Still, so the panel looks solid and is drawn again only when something changes.
        const double rr = 0.75 * pxPerDeg;
        Disc(p, x, y, rr, rr - 0.12 * pxPerDeg, ink, ink, ink, 0.6);
        Disc(p, x, y, core, 0, ink, ink, ink, 1);
    } else if (p.state == "capture") {
        const double rr = 0.75 * pxPerDeg;
        Disc(p, x, y, rr, rr - 0.12 * pxPerDeg, ink, ink, ink, 0.25);
        Disc(p, x, y, rr, rr - 0.12 * pxPerDeg, 0.3, 0.85, 1.0, 1, std::clamp(p.progress, 0.0, 1.0));
        Disc(p, x, y, core, 0, ink, ink, ink, 1);
    } else if (p.state == "done") {
        Disc(p, x, y, 0.75 * pxPerDeg, 0, 0.25, 0.85, 0.4, 0.9);
        Disc(p, x, y, core, 0, 1, 1, 1, 1);
    } else if (p.state == "fail") {
        Disc(p, x, y, 0.75 * pxPerDeg, 0.6 * pxPerDeg, 0.95, 0.35, 0.3, 0.9);
        Disc(p, x, y, core, 0, ink, ink, ink, 1);
    }
}


// ---------------------------------------------------------------- the buffers (see the top)

struct Buffer {
    gbm_bo *bo = nullptr;
    int fd = -1;
    vr::SharedTextureHandle_t handle = 0;
};

struct Buffers {
    int drm = -1;
    gbm_device *gbm = nullptr;
    Buffer b[3];
    int next = 0;
    bool ok = false;

    bool Make() {
        drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (drm >= 0) gbm = gbm_create_device(drm);
        for (Buffer &x : b) {
            // ABGR8888 is R, G, B, A in memory, like Panel::px.
            if (gbm) x.bo = gbm_bo_create(gbm, kMaxW, kMaxH, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (!x.bo || (x.fd = gbm_bo_get_fd(x.bo)) < 0) break;
            vr::DmabufAttributes_t a{};
            a.unWidth = kMaxW, a.unHeight = kMaxH;
            a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
            a.unFormat = DRM_FORMAT_ABGR8888;
            a.ulModifier = DRM_FORMAT_MOD_LINEAR;
            a.unPlaneCount = 1;
            a.plane[0].unOffset = gbm_bo_get_offset(x.bo, 0);
            a.plane[0].unStride = gbm_bo_get_stride(x.bo);
            a.plane[0].nFd = x.fd;
            if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &x.handle)) x.handle = 0;
            if (!x.handle) break;
        }
        ok = b[2].handle != 0;
        if (!ok) {
            std::fprintf(stderr, "ft-gazepanel: no shared buffers; falling back to SetOverlayRaw (it flickers)\n");
            Drop();
        }
        return ok;
    }

    void Drop() {
        for (Buffer &x : b) {
            if (x.handle) vr::VRIPCResourceManager()->UnrefResource(x.handle);
            if (x.fd >= 0) close(x.fd);
            if (x.bo) gbm_bo_destroy(x.bo);
            x = Buffer{};
        }
        if (gbm) gbm_device_destroy(gbm);
        if (drm >= 0) close(drm);
        gbm = nullptr, drm = -1, ok = false;
    }

    // p's picture to the overlay: into the next buffer, premultiplied (the overlay's flag says
    // so), then the overlay switches to it.
    void Present(vr::IVROverlay *ov, vr::VROverlayHandle_t h, const Panel &p) {
        vr::EVROverlayError e;
        if (!ok) {
            e = ov->SetOverlayRaw(h, const_cast<uint8_t *>(p.px.data()), uint32_t(p.w), uint32_t(p.h), 4);
        } else {
            Buffer &x = b[next];
            next = (next + 1) % 3;
            uint32_t stride = 0;
            void *mapping = nullptr;
            auto *dst = static_cast<uint8_t *>(gbm_bo_map(x.bo, 0, 0, p.w, p.h, GBM_BO_TRANSFER_WRITE, &stride, &mapping));
            if (!dst) {
                std::fprintf(stderr, "ft-gazepanel: can't map a buffer\n");
                return;
            }
            for (int y = 0; y < p.h; ++y) {
                const uint8_t *src = &p.px[size_t(y) * p.w * 4];
                uint8_t *row = dst + size_t(y) * stride;
                for (int i = 0; i < p.w * 4; i += 4) {
                    const unsigned a = src[i + 3];
                    row[i] = uint8_t(src[i] * a / 255), row[i + 1] = uint8_t(src[i + 1] * a / 255);
                    row[i + 2] = uint8_t(src[i + 2] * a / 255), row[i + 3] = uint8_t(a);
                }
            }
            gbm_bo_unmap(x.bo, mapping);
            const vr::VRTextureBounds_t bounds{0, 0, float(p.w) / kMaxW, float(p.h) / kMaxH};
            ov->SetOverlayTextureBounds(h, &bounds);
            vr::Texture_t tex = {&x.handle, vr::TextureType_SharedTextureHandle, vr::ColorSpace_Gamma};
            e = ov->SetOverlayTexture(h, &tex);
        }
        if (e != vr::VROverlayError_None)
            std::fprintf(stderr, "ft-gazepanel: the picture didn't go to SteamVR: %s\n", ov->GetOverlayErrorNameFromEnum(e));
    }
};

}  // namespace

int main(int argc, char **argv) {
    bool watchStdin = false;
    std::string sockName = "ft_gazepanel";
    double distance = 1.5;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--watch-stdin")) watchStdin = true;
        else if (!std::strcmp(argv[i], "--socket") && i + 1 < argc) sockName = argv[++i];
        else if (!std::strcmp(argv[i], "--distance") && i + 1 < argc) distance = std::clamp(std::atof(argv[++i]), 0.5, 5.0);
        else {
            std::fprintf(stderr, "usage: %s [--watch-stdin] [--socket NAME] [--distance METRES]\n", argv[0]);
            return 2;
        }
    }
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
    if (watchStdin)
        std::thread([] {
            char c[256];
            while (read(0, c, sizeof c) > 0) {
            }
            g_stop = true;
        }).detach();

    int sock = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::memcpy(addr.sun_path + 1, sockName.data(), std::min(sockName.size(), sizeof addr.sun_path - 2));
    if (bind(sock, reinterpret_cast<sockaddr *>(&addr), socklen_t(offsetof(sockaddr_un, sun_path) + 1 + sockName.size())) != 0) {
        std::fprintf(stderr, "ft-gazepanel: @%s is taken (another copy running?)\n", sockName.c_str());
        return 1;
    }

    // As Frametop's other SteamVR clients: background first, so we never start vrserver.
    vr::EVRInitError err = vr::VRInitError_None;
    vr::VR_Init(&err, vr::VRApplication_Background);
    if (err == vr::VRInitError_None) {
        vr::VR_Shutdown();
        vr::VR_Init(&err, vr::VRApplication_Overlay);
    }
    if (err != vr::VRInitError_None) {
        std::fprintf(stderr, "ft-gazepanel: SteamVR: %s\n", vr::VR_GetVRInitErrorAsEnglishDescription(err));
        return 1;
    }
    vr::IVROverlay *ov = vr::VROverlay();
    vr::VROverlayHandle_t h = vr::k_ulOverlayHandleInvalid;
    if (ov->CreateOverlay("frametop.gazepanel", "Frametop gaze calibration", &h) != vr::VROverlayError_None) {
        std::fprintf(stderr, "ft-gazepanel: can't create the overlay (another copy running?)\n");
        return 1;
    }
    ov->SetOverlaySortOrder(h, 250);  // in front of Frametop's screens and the pointer's dot
    LoadFont();
    Buffers buffers;
    if (buffers.Make()) ov->SetOverlayFlag(h, vr::VROverlayFlags_IsPremultiplied, true);

    Panel p;
    bool visible = false, dirty = false, shown = false;  // shown: SteamVR shows it (after its first picture)
    auto place = [&] {
        const double half = std::tan(p.wDeg * M_PI / 360);
        vr::HmdMatrix34_t m{};
        m.m[0][0] = m.m[1][1] = m.m[2][2] = 1;
        m.m[2][3] = float(-distance);
        ov->SetOverlayTransformTrackedDeviceRelative(h, vr::k_unTrackedDeviceIndex_Hmd, &m);
        ov->SetOverlayWidthInMeters(h, float(2 * distance * half));
    };
    std::fprintf(stderr, "ft-gazepanel running: @%s, %.2f m\n", sockName.c_str(), distance);

    while (!g_stop) {
        char buf[512];
        sockaddr_un from{};
        socklen_t fromLen = sizeof from;
        ssize_t n;
        while ((n = recvfrom(sock, buf, sizeof buf - 1, 0, reinterpret_cast<sockaddr *>(&from), &fromLen)) > 0) {
            buf[n] = 0;
            std::string reply = "ok";
            char word[16] = "", state[16] = "";
            double a = 0, b = 0, c = 0, d = 0, e = 0;
            int rest = 0;
            if (!std::strncmp(buf, "show ", 5)) {
                p.full = !std::strcmp(buf + 5, "full");
                p.fit = !std::strcmp(buf + 5, "fit");
                const bool five = !std::strcmp(buf + 5, "five");
                p.w = p.full ? kFullW : p.fit ? kFitW : five ? kFiveW : kQuickPx;
                p.h = p.full ? kFullH : p.fit ? kFitH : five ? kFiveH : kQuickPx;
                p.wDeg = p.full ? kFullDeg : p.fit ? kFitDeg : five ? kFiveDeg : kQuickDeg;
                p.title.clear(), p.text.clear(), p.note.clear(), p.dotOn = false, p.state = "off";
                p.eyes[0] = p.eyes[1] = EyeCard{}, p.hints.clear();
                place();
                visible = dirty = true;  // shown with its first picture
            } else if (!std::strcmp(buf, "hide")) {
                ov->HideOverlay(h);
                visible = shown = false;
            } else if (std::sscanf(buf, "bg %lf", &a) == 1) {
                p.bg = std::clamp(a, 0.0, 1.0), dirty = true;
            } else if (std::sscanf(buf, "dot %lf %lf %15s %lf", &a, &b, state, &c) >= 3) {
                p.dotYaw = a, p.dotPitch = b, p.state = state, p.progress = c;
                p.dotOn = std::strcmp(state, "off") != 0, dirty = true;
            } else if (int k; std::sscanf(buf, "eye %d %lf %lf %lf %lf %lf %n", &k, &a, &b, &c, &d, &e, &rest) >= 6 &&
                       rest > 0 && (k == 0 || k == 1)) {
                p.eyes[k] = EyeCard{buf + rest, a, b, c, d, e}, dirty = true;
            } else if (!std::strncmp(buf, "hints", 5)) {
                p.hints.clear();
                std::string s = buf[5] == ' ' ? buf + 6 : "";
                for (size_t at = 0; !s.empty() && at <= s.size();) {
                    const size_t bar = std::min(s.find('|', at), s.size());
                    p.hints.push_back(s.substr(at, bar - at));
                    at = bar + 1;
                }
                dirty = true;
            } else if (!std::strncmp(buf, "title", 5)) {
                p.title = buf[5] == ' ' ? buf + 6 : "", dirty = true;
            } else if (!std::strncmp(buf, "text", 4)) {
                p.text = buf[4] == ' ' ? buf + 5 : "", dirty = true;
            } else if (!std::strncmp(buf, "note", 4)) {
                p.note = buf[4] == ' ' ? buf + 5 : "", dirty = true;
            } else if (std::sscanf(buf, "%15s", word) == 1 && !std::strcmp(word, "ping")) {
                reply = visible ? "ok shown" : "ok hidden";
            } else {
                reply = "error unknown command";
            }
            if (fromLen > offsetof(sockaddr_un, sun_path))
                sendto(sock, reply.data(), reply.size(), 0, reinterpret_cast<sockaddr *>(&from), fromLen);
            fromLen = sizeof from;
        }
        vr::VREvent_t ev;
        while (vr::VRSystem()->PollNextEvent(&ev, sizeof ev))
            if (ev.eventType == vr::VREvent_Quit) {
                vr::VRSystem()->AcknowledgeQuit_Exiting();
                g_stop = true;
            }
        if (visible && dirty) {
            Draw(p);
            buffers.Present(ov, h, p);
            if (!shown) ov->ShowOverlay(h), shown = true;
            dirty = false;
        }
        // Until a command comes, or 10 ms while shown (SteamVR's events). Hidden, it waits up to a
        // second: it woke 20 to 30 times a second for nothing, the main cost left with gaze idle.
        // A closed stdin (--watch-stdin) wakes it too, so quitting doesn't wait.
        pollfd fds[2] = {{sock, POLLIN, 0}, {0, POLLIN, 0}};
        poll(fds, watchStdin ? 2 : 1, visible ? 10 : 1000);
    }
    ov->DestroyOverlay(h);
    buffers.Drop();
    vr::VR_Shutdown();
    return 0;
}
