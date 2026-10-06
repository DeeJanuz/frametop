// Hand cutouts (see handcut.h).
#include "handcut.h"

#include "../hands/include/fh_hands.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace handcut {
namespace {

// The hands file ft-hands publishes (hands/include/fh_hands.h).
constexpr uint32_t kMaxHands = FH_HANDS_MAX_HANDS, kMaxCapsules = FH_HANDS_MAX_CAPSULES;
constexpr int64_t kStaleNs = 300'000'000;   // hands older than this are gone
constexpr int64_t kHistoryNs = 1'000'000'000;
constexpr double kNear = 0.12;   // metres: nothing closer to an eye than this is cut
constexpr double kMaxSpeed = 2.5;     // m/s: faster is a tracking jump, not a hand
constexpr double kStillSpeed = 0.05;  // m/s: below this, a hand's velocity is noise
constexpr double kMaxAhead = 0.12;    // s: never predict further than this

int64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

void Apply(const Mat &m, const float p[3], float out[3]) {
    for (int i = 0; i < 3; ++i) out[i] = m.m[i][0] * p[0] + m.m[i][1] * p[1] + m.m[i][2] * p[2] + m.m[i][3];
}

// Room -> panel-local: R^T (p - t).
void ToLocal(const Mat &m, const double p[3], double out[3]) {
    const double d[3] = {p[0] - m.m[0][3], p[1] - m.m[1][3], p[2] - m.m[2][3]};
    for (int i = 0; i < 3; ++i) out[i] = m.m[0][i] * d[0] + m.m[1][i] * d[1] + m.m[2][i] * d[2];
}

}  // namespace

// ------------------------------------------------------------------------------- hands

bool Hands::Read() {
    if (!map_) {
        const int64_t now = MonoNs();
        if (now - lastOpenTry_ < 1'000'000'000) return false;
        lastOpenTry_ = now;
        const std::string path = "/run/user/" + std::to_string(getuid()) + "/frametop-hands/hands";
        fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd_ < 0) return false;
        struct stat st;
        if (fstat(fd_, &st) < 0 || st.st_uid != getuid() || size_t(st.st_size) < sizeof(fh_hands_t)) {
            close(fd_), fd_ = -1;
            return false;
        }
        void *m = mmap(nullptr, sizeof(fh_hands_t), PROT_READ, MAP_SHARED, fd_, 0);
        if (m == MAP_FAILED) {
            close(fd_), fd_ = -1;
            return false;
        }
        map_ = m;
    }
    const auto *file = static_cast<const fh_hands_t *>(map_);
    const auto *seq = const_cast<const uint64_t *>(&file->seq);
    const uint64_t s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if ((s1 & 1) || s1 == seq_) return false;
    fh_hands_t copy;
    std::memcpy(static_cast<void *>(&copy), map_, sizeof copy);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(seq, __ATOMIC_RELAXED) != s1 || std::memcmp(copy.magic, FH_HANDS_MAGIC, 8) != 0) return false;
    seq_ = s1;
    const uint32_t nhands = std::min<uint32_t>(copy.nhands, kMaxHands), ncaps = std::min<uint32_t>(copy.ncapsules, kMaxCapsules);
    captureNs_ = int64_t(copy.capture_ns), publishNs_ = int64_t(copy.publish_ns);
    const Mat head = HeadAt(captureNs_);

    // each hand's palm in the room, and its velocity from the last time it was seen
    ids_.clear();
    std::vector<int> owners;   // the hand each capsule belongs to, in file order
    for (uint32_t k = 0; k < nhands; ++k) {
        const fh_hand_t &h = copy.hands[k];
        const uint32_t id = h.id;
        const auto &pts = h.pts;
        const int idx = int(ids_.size());
        ids_.push_back(id);
        owners.insert(owners.end(), std::min<uint32_t>(h.ncapsules, kMaxCapsules), idx);
        double palm[3] = {0, 0, 0};
        bool ok = true;
        for (int j : {0, 5, 9, 13, 17}) {
            float w[3];
            ok = ok && std::isfinite(pts[j][0]) && std::isfinite(pts[j][1]) && std::isfinite(pts[j][2]);
            Apply(head, pts[j], w);
            for (int i = 0; i < 3; ++i) palm[i] += w[i] / 5;
        }
        Motion &m = motion_[id];
        const double dt = (captureNs_ - m.ns) / 1e9;
        if (!ok) {
            m = Motion{};
            continue;
        }
        if (m.ns && dt > 0.005 && dt < 0.2) {
            double speed = 0;
            for (int i = 0; i < 3; ++i) {
                m.v[i] += 0.5 * ((palm[i] - m.palm[i]) / dt - m.v[i]);
                speed += m.v[i] * m.v[i];
            }
            speed = std::sqrt(speed);
            if (speed > kMaxSpeed)
                for (double &v : m.v) v *= kMaxSpeed / speed;
        } else {
            m.v[0] = m.v[1] = m.v[2] = 0;
        }
        m.ns = captureNs_;
        std::memcpy(m.palm, palm, sizeof palm);
    }
    for (auto it = motion_.begin(); it != motion_.end();)
        it = captureNs_ - it->second.ns > kStaleNs ? motion_.erase(it) : std::next(it);
    if (owners.size() != ncaps) owners.assign(ncaps, -1);

    base_.clear(), owner_.clear();
    for (uint32_t k = 0; k < ncaps; ++k) {
        const fh_capsule_t &f = copy.capsules[k];
        bool ok = true;
        for (float v : {f.a[0], f.a[1], f.a[2], f.b[0], f.b[1], f.b[2], f.ra, f.rb}) ok = ok && std::isfinite(v) && std::fabs(v) < 10;
        if (!ok || f.ra <= 0 || f.rb <= 0) continue;
        Capsule c;
        Apply(head, f.a, c.a);
        Apply(head, f.b, c.b);
        c.ra = f.ra, c.rb = f.rb;
        base_.push_back(c);
        owner_.push_back(owners[k]);
    }
    return true;
}

void Hands::SetPrediction(bool on, double leadMs) {
    predict_ = on;
    leadNs_ = int64_t(std::clamp(leadMs, 0.0, 100.0) * 1e6);
}

Mat Hands::HeadAt(int64_t ns) const {
    const Past *best = nullptr;
    for (const Past &p : history_)
        if (!best || std::llabs(p.ns - ns) < std::llabs(best->ns - ns)) best = &p;
    return best ? best->head : Mat{};
}

bool Hands::Update(const Mat &head, int64_t nowNs) {
    history_.push_back({nowNs, head});
    while (!history_.empty() && nowNs - history_.front().ns > kHistoryNs) history_.erase(history_.begin());
    Read();
    if (nowNs - publishNs_ > kStaleNs) base_.clear(), owner_.clear();
    // move each hand ahead to when this frame will be on the displays; a slow hand's
    // velocity is mostly tracking noise, so it fades out below kStillSpeed
    const double ahead = std::clamp((nowNs + leadNs_ - captureNs_) / 1e9, 0.0, kMaxAhead);
    world_ = base_;
    for (size_t k = 0; predict_ && k < world_.size(); ++k) {
        if (owner_[k] < 0) continue;
        const auto m = motion_.find(ids_[owner_[k]]);
        if (m == motion_.end()) continue;
        const double *v = m->second.v;
        const double speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        const double gain = std::clamp((speed - kStillSpeed) / kStillSpeed, 0.0, 1.0);
        for (int i = 0; i < 3; ++i) {
            world_[k].a[i] += float(v[i] * gain * ahead);
            world_[k].b[i] += float(v[i] * gain * ahead);
        }
    }
    return !world_.empty();
}

void EyePositions(const Mat &head, double out[2][3]) {
    const vr::EVREye eyes[2] = {vr::Eye_Left, vr::Eye_Right};
    for (int e = 0; e < 2; ++e) {
        const Mat t = vr::VRSystem()->GetEyeToHeadTransform(eyes[e]);
        for (int i = 0; i < 3; ++i)
            out[e][i] = head.m[i][0] * t.m[0][3] + head.m[i][1] * t.m[1][3] + head.m[i][2] * t.m[2][3] + head.m[i][3];
    }
}

// ----------------------------------------------------------------------------- project

namespace {

// Where the line from eye e through point q (both panel-local) meets the panel, as texture
// pixels, and how much a size at q grows there. False if q isn't between the eye and it.
bool OnPanel(const Panel &p, const double e[3], const double q[3], double *x, double *y, double *grow) {
    const double d[3] = {q[0] - e[0], q[1] - e[1], q[2] - e[2]};
    double s, u, v;
    if (p.curve <= 0) {
        if (e[2] <= q[2] || q[2] <= 0) return false;
        s = e[2] / (e[2] - q[2]);
        u = e[0] + s * d[0];
        v = e[1] + s * d[1];
    } else {
        // OpenVR bends a curved panel into a cylinder around (0, *, r), toward its front.
        const double r = p.curve, ez = e[2] - r;
        const double A = d[0] * d[0] + d[2] * d[2], B = 2 * (e[0] * d[0] + ez * d[2]), C = e[0] * e[0] + ez * ez - r * r;
        const double disc = B * B - 4 * A * C;
        if (A < 1e-12 || disc < 0) return false;
        s = (-B + std::sqrt(disc)) / (2 * A);   // the far side: the panel, seen from inside
        const double px = e[0] + s * d[0], pz = e[2] + s * d[2];
        if (pz > r) return false;
        u = r * std::atan2(px, r - pz);
        v = e[1] + s * d[1];
    }
    if (s <= 1) return false;  // the hand is behind the panel
    *x = (u / p.width + 0.5) * p.pxWidth;
    *y = (0.5 - v / p.height) * p.pxHeight;
    *grow = s;
    return true;
}

}  // namespace

bool Project(const Panel &p, const std::vector<Capsule> &caps, const double eyes[2][3], std::vector<Capsule2D> out[2]) {
    const double pxPerM = p.pxWidth / p.width;
    bool any = false;
    for (int e = 0; e < 2; ++e) {
        out[e].clear();
        double eye[3];
        ToLocal(p.pose, eyes[e], eye);
        if (eye[2] <= 0.01) continue;  // behind the panel
        for (const Capsule &c : caps) {
            double a[3], b[3];
            const double wa[3] = {c.a[0], c.a[1], c.a[2]}, wb[3] = {c.b[0], c.b[1], c.b[2]};
            ToLocal(p.pose, wa, a);
            ToLocal(p.pose, wb, b);
            // a hand pushed through the panel: keep the part in front
            const double eps = 0.002;
            if (a[2] < eps && b[2] < eps) continue;
            if (a[2] < eps || b[2] < eps) {
                double *in = a[2] < eps ? b : a, *out3 = a[2] < eps ? a : b;
                const double t = (in[2] - eps) / (in[2] - out3[2]);
                for (int i = 0; i < 3; ++i) out3[i] = in[i] + t * (out3[i] - in[i]);
            }
            // and the part near the eye's plane: it would land far across the panel with a
            // huge radius, so one bad hand estimate there tears a hole through the screen
            const double zmax = eye[2] - kNear;
            if (a[2] > zmax && b[2] > zmax) continue;
            if (a[2] > zmax || b[2] > zmax) {
                double *in = a[2] > zmax ? b : a, *out3 = a[2] > zmax ? a : b;
                const double t = (zmax - in[2]) / (out3[2] - in[2]);
                for (int i = 0; i < 3; ++i) out3[i] = in[i] + t * (out3[i] - in[i]);
            }
            double ax, ay, ga, bx, by, gb;
            if (!OnPanel(p, eye, a, &ax, &ay, &ga) || !OnPanel(p, eye, b, &bx, &by, &gb)) continue;
            const float ra = float(c.ra * ga * pxPerM), rb = float(c.rb * gb * pxPerM);
            const float r = std::max(ra, rb);
            if (std::max(ax, bx) + r < 0 || std::min(ax, bx) - r > p.pxWidth || std::max(ay, by) + r < 0 ||
                std::min(ay, by) - r > p.pxHeight)
                continue;
            out[e].push_back({float(ax), float(ay), float(bx), float(by), ra, rb});
            any = true;
        }
    }
    return any;
}

// ---------------------------------------------------------------------------- renderer

namespace {

PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;
PFNEGLCREATEIMAGEKHRPROC pCreateImage;
PFNEGLDESTROYIMAGEKHRPROC pDestroyImage;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pImageTargetRenderbuffer;
PFNEGLCREATESYNCKHRPROC pCreateSync;
PFNEGLDESTROYSYNCKHRPROC pDestroySync;
PFNEGLCLIENTWAITSYNCKHRPROC pClientWaitSync;

const char *kVertex = R"(
attribute vec2 pos;          // the unit square
uniform vec4 rect;           // where it goes, in pixels of the eye's half: x0 y0 x1 y1
uniform vec2 size;           // the half's size in pixels
varying vec2 px;
varying vec2 uv;
void main() {
    px = mix(rect.xy, rect.zw, pos);
    uv = px / size;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
})";

// The client's pixels, opaque (its alpha is ignored, as IgnoreTextureAlpha did).
const char *kCopy = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;   // mediump (16-bit on Adreno) steps 1.7 texels across a 3440-pixel screen
uniform samplerExternalOES tex;
varying vec2 uv;
void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); })";

// Coverage of one tapered capsule; blended to take that much alpha away.
const char *kCut = R"(
precision highp float;
uniform vec2 a, b, r;
uniform float feather;
varying vec2 px;
void main() {
    vec2 ab = b - a;
    float t = clamp(dot(px - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
    float d = length(px - (a + t * ab));
    float rad = mix(r.x, r.y, t);
    gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0 - smoothstep(rad - feather, rad + feather, d));
})";

unsigned Shader(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "handcut: shader: %s\n", log);
    }
    return s;
}

unsigned Program(const char *fs) {
    const GLuint p = glCreateProgram();
    glAttachShader(p, Shader(GL_VERTEX_SHADER, kVertex));
    glAttachShader(p, Shader(GL_FRAGMENT_SHADER, fs));
    glBindAttribLocation(p, 0, "pos");
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        std::fprintf(stderr, "handcut: program: %s\n", log);
        return 0;
    }
    return p;
}

EGLImageKHR ImageFor(EGLDisplay dpy, const ft_dmabuf &b) {
    static const EGLint fd[4] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE2_FD_EXT,
                                 EGL_DMA_BUF_PLANE3_FD_EXT};
    static const EGLint off[4] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
                                  EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT};
    static const EGLint pitch[4] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
                                    EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT};
    static const EGLint lo[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
                                 EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT};
    static const EGLint hi[4] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
                                 EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT};
    EGLint a[64];
    int n = 0;
    a[n++] = EGL_WIDTH, a[n++] = b.width, a[n++] = EGL_HEIGHT, a[n++] = b.height;
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT, a[n++] = EGLint(b.format);
    for (int i = 0; i < b.n_planes && i < 4; ++i) {
        a[n++] = fd[i], a[n++] = b.fd[i], a[n++] = off[i], a[n++] = EGLint(b.offset[i]);
        a[n++] = pitch[i], a[n++] = EGLint(b.stride[i]);
        if (b.modifier != DRM_FORMAT_MOD_INVALID) {
            a[n++] = lo[i], a[n++] = EGLint(b.modifier & 0xffffffff);
            a[n++] = hi[i], a[n++] = EGLint(b.modifier >> 32);
        }
    }
    a[n++] = EGL_NONE;
    return pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
}

}  // namespace

Renderer::~Renderer() {
    for (auto &[k, r] : rings_)
        for (Output &o : r.out) FreeOutput(o);
    for (auto &[k, im] : imported_) {
        glDeleteTextures(1, &im.tex);
        pDestroyImage(EGLDisplay(dpy_), EGLImageKHR(im.image));
    }
    if (ctx_) eglDestroyContext(EGLDisplay(dpy_), EGLContext(ctx_));
    if (dpy_) eglTerminate(EGLDisplay(dpy_));
    if (gbm_) gbm_device_destroy(static_cast<gbm_device *>(gbm_));
    if (drm_ >= 0) close(drm_);
}

bool Renderer::Init(const std::vector<uint64_t> &modifiers, std::function<void(const Output *)> released) {
    if (ready_) return true;
    modifiers_ = modifiers;
    released_ = std::move(released);
    drm_ = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_ < 0) return std::perror("handcut: /dev/dri/renderD128"), false;
    gbm_ = gbm_create_device(drm_);
    pGetPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
    pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    pDestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    pImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    pImageTargetRenderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    pCreateSync = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    pDestroySync = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    pClientWaitSync = reinterpret_cast<PFNEGLCLIENTWAITSYNCKHRPROC>(eglGetProcAddress("eglClientWaitSyncKHR"));
    if (!gbm_ || !pGetPlatformDisplay || !pCreateImage || !pImageTargetTexture || !pImageTargetRenderbuffer ||
        !pCreateSync || !pDestroySync || !pClientWaitSync) {
        std::fprintf(stderr, "handcut: GBM or EGL extensions missing\n");
        return false;
    }
    EGLDisplay dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_, nullptr);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return std::fprintf(stderr, "handcut: no EGL display\n"), false;
    dpy_ = dpy;
    eglBindAPI(EGL_OPENGL_ES_API);
    const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
    EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
        return std::fprintf(stderr, "handcut: no surfaceless GLES context\n"), false;
    ctx_ = ctx;
    copyProg_ = Program(kCopy);
    cutProg_ = Program(kCut);
    if (!copyProg_ || !cutProg_) return false;
    const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
    glGenBuffers(1, &vbo_);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
    ready_ = true;
    return true;
}

unsigned Renderer::Texture(const void *key, const ft_dmabuf &src) {
    auto it = imported_.find(key);
    if (it != imported_.end()) return it->second.tex;
    EGLImageKHR image = ImageFor(EGLDisplay(dpy_), src);
    if (image == EGL_NO_IMAGE_KHR) {
        std::fprintf(stderr, "handcut: can't import a %dx%d client buffer (format 0x%x modifier 0x%llx)\n", src.width,
                     src.height, src.format, (unsigned long long)src.modifier);
        return 0;
    }
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, image);
    imported_[key] = {image, tex};
    return tex;
}

void Renderer::Forget(const void *key) {
    auto it = imported_.find(key);
    if (it == imported_.end()) return;
    glDeleteTextures(1, &it->second.tex);
    pDestroyImage(EGLDisplay(dpy_), EGLImageKHR(it->second.image));
    imported_.erase(it);
    for (auto &[k, r] : rings_)   // a new buffer at the same address isn't this one
        for (Output &o : r.out)
            if (o.key == key) o.drawn = false;
}

bool Renderer::MakeOutput(Output &o, int w, int h) {
    auto *gbm = static_cast<gbm_device *>(gbm_);
    std::vector<uint64_t> mods;
    for (uint64_t m : modifiers_)
        if (m != DRM_FORMAT_MOD_INVALID) mods.push_back(m);
    gbm_bo *bo = mods.empty() ? gbm_bo_create(gbm, w, h, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR)
                              : gbm_bo_create_with_modifiers2(gbm, w, h, GBM_FORMAT_ABGR8888, mods.data(),
                                                             unsigned(mods.size()), GBM_BO_USE_RENDERING);
    if (!bo) return std::fprintf(stderr, "handcut: can't allocate a %dx%d output\n", w, h), false;
    o.bo = bo;
    o.buf = {};
    o.buf.width = w, o.buf.height = h;
    o.buf.format = DRM_FORMAT_ABGR8888;
    o.buf.modifier = mods.empty() ? DRM_FORMAT_MOD_LINEAR : gbm_bo_get_modifier(bo);
    o.buf.n_planes = gbm_bo_get_plane_count(bo);
    for (int i = 0; i < o.buf.n_planes && i < 4; ++i) {
        o.buf.fd[i] = gbm_bo_get_fd_for_plane(bo, i);
        o.buf.offset[i] = gbm_bo_get_offset(bo, i);
        o.buf.stride[i] = gbm_bo_get_stride_for_plane(bo, i);
    }
    EGLImageKHR image = ImageFor(EGLDisplay(dpy_), o.buf);
    if (image == EGL_NO_IMAGE_KHR) return std::fprintf(stderr, "handcut: can't render to the output\n"), FreeOutput(o), false;
    o.image = image;
    glGenRenderbuffers(1, &o.rb);
    glBindRenderbuffer(GL_RENDERBUFFER, o.rb);
    pImageTargetRenderbuffer(GL_RENDERBUFFER, image);
    glGenFramebuffers(1, &o.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, o.rb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        return std::fprintf(stderr, "handcut: output framebuffer incomplete\n"), FreeOutput(o), false;
    return true;
}

void Renderer::FreeOutput(Output &o) {
    if (o.bo && released_) released_(&o);
    if (o.fence) pDestroySync(EGLDisplay(dpy_), EGLSyncKHR(o.fence));
    if (o.fbo) glDeleteFramebuffers(1, &o.fbo);
    if (o.rb) glDeleteRenderbuffers(1, &o.rb);
    if (o.image) pDestroyImage(EGLDisplay(dpy_), EGLImageKHR(o.image));
    for (int i = 0; i < o.buf.n_planes && i < 4; ++i)
        if (o.buf.fd[i] >= 0) close(o.buf.fd[i]);
    if (o.bo) gbm_bo_destroy(static_cast<gbm_bo *>(o.bo));
    o = Output{};
}

void Renderer::DropPanel(int panel) {
    auto it = rings_.find(panel);
    if (it == rings_.end()) return;
    for (Output &o : it->second.out) FreeOutput(o);
    rings_.erase(it);
}

namespace {

// The pixels a cutout's quad covers (see Draw), as x0 y0 x1 y1 in the eye's half.
void Bounds(const Capsule2D &c, float b[4]) {
    const float feather = std::max(1.5f, 0.15f * std::min(c.ra, c.rb));
    const float r = std::max(c.ra, c.rb) + feather;
    b[0] = std::min(c.ax, c.bx) - r, b[1] = std::min(c.ay, c.by) - r;
    b[2] = std::max(c.ax, c.bx) + r, b[3] = std::max(c.ay, c.by) + r;
}

// Within a quarter pixel: the same picture.
bool SameSpots(const std::vector<Capsule2D> a[2], const std::vector<Capsule2D> b[2]) {
    for (int e = 0; e < 2; ++e) {
        if (a[e].size() != b[e].size()) return false;
        for (size_t i = 0; i < a[e].size(); ++i) {
            const Capsule2D &p = a[e][i], &q = b[e][i];
            for (float d : {p.ax - q.ax, p.ay - q.ay, p.bx - q.bx, p.by - q.by, p.ra - q.ra, p.rb - q.rb})
                if (std::fabs(d) > 0.25f) return false;
        }
    }
    return true;
}

int64_t SteadyNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool Renderer::Passed(Output &o, int64_t timeoutNs) {
    if (!o.fence) return true;
    const EGLint r = pClientWaitSync(EGLDisplay(dpy_), EGLSyncKHR(o.fence), 0, EGLTimeKHR(timeoutNs));
    if (r == EGL_TIMEOUT_EXPIRED_KHR) return false;
    pDestroySync(EGLDisplay(dpy_), EGLSyncKHR(o.fence));   // passed, or failed: don't wait on it again
    o.fence = nullptr;
    return true;
}

// Draws one buffer. Partial: the buffer holds this client frame already, with o.spots cut
// out, so each eye is drawn again only inside the box around those and the new cutouts.
void Renderer::Draw(Output &o, unsigned tex, int w, int h, const std::vector<Capsule2D> eyes[2], bool partial) {
    glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    for (int e = 0; e < 2; ++e) {
        if (partial) {
            float box[4] = {1e9f, 1e9f, -1e9f, -1e9f}, b[4];
            const std::vector<Capsule2D> *lists[2] = {&o.spots[e], &eyes[e]};
            for (const std::vector<Capsule2D> *list : lists)
                for (const Capsule2D &c : *list) {
                    Bounds(c, b);
                    box[0] = std::min(box[0], b[0]), box[1] = std::min(box[1], b[1]);
                    box[2] = std::max(box[2], b[2]), box[3] = std::max(box[3], b[3]);
                }
            // Window y is the buffer's row, the same way down as the cutouts' y (see kVertex).
            const int x0 = std::clamp(int(std::floor(box[0])) - 1, 0, w), y0 = std::clamp(int(std::floor(box[1])) - 1, 0, h);
            const int x1 = std::clamp(int(std::ceil(box[2])) + 1, 0, w), y1 = std::clamp(int(std::ceil(box[3])) + 1, 0, h);
            if (x1 <= x0 || y1 <= y0) continue;   // no cutout in this eye, then or now
            glEnable(GL_SCISSOR_TEST);
            glScissor(e * w + x0, y0, x1 - x0, y1 - y0);
        }
        glViewport(e * w, 0, w, h);
        glDisable(GL_BLEND);
        glUseProgram(copyProg_);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glUniform1i(glGetUniformLocation(copyProg_, "tex"), 0);
        glUniform4f(glGetUniformLocation(copyProg_, "rect"), 0, 0, float(w), float(h));
        glUniform2f(glGetUniformLocation(copyProg_, "size"), float(w), float(h));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        // take alpha away where the hand is; the colour stays (straight alpha)
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(cutProg_);
        glUniform2f(glGetUniformLocation(cutProg_, "size"), float(w), float(h));
        const GLint uRect = glGetUniformLocation(cutProg_, "rect"), uA = glGetUniformLocation(cutProg_, "a"),
                    uB = glGetUniformLocation(cutProg_, "b"), uR = glGetUniformLocation(cutProg_, "r"),
                    uF = glGetUniformLocation(cutProg_, "feather");
        for (const Capsule2D &c : eyes[e]) {
            float b[4];
            Bounds(c, b);
            glUniform4f(uRect, b[0], b[1], b[2], b[3]);
            glUniform2f(uA, c.ax, c.ay);
            glUniform2f(uB, c.bx, c.by);
            glUniform2f(uR, c.ra, c.rb);
            glUniform1f(uF, std::max(1.5f, 0.15f * std::min(c.ra, c.rb)));
            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
        glDisable(GL_SCISSOR_TEST);
    }
    glDisable(GL_BLEND);
}

const Output *Renderer::Composite(int panel, const void *key, uint64_t serial, const ft_dmabuf &src,
                                  const std::vector<Capsule2D> eyes[2]) {
    if (!ready_) return nullptr;
    const int64_t t0 = SteadyNs();
    const int w = src.width, h = src.height;
    Ring &ring = rings_[panel];
    if (ring.w != w || ring.h != h) {
        for (Output &old : ring.out) FreeOutput(old);
        ring.w = w, ring.h = h, ring.shown = ring.before = ring.drawing = -1;
    }
    // After a pause the panel showed its client buffer, so nothing of ours is on it.
    if (t0 - ring.lastCall > 30'000'000) ring.shown = ring.before = -1;
    ring.lastCall = t0;
    auto promote = [&ring] {
        ring.before = ring.shown, ring.shown = ring.drawing, ring.drawing = -1;
    };
    if (ring.drawing >= 0 && Passed(ring.out[ring.drawing], 0)) promote();

    const int newest = ring.drawing >= 0 ? ring.drawing : ring.shown;
    const Output *n = newest >= 0 ? &ring.out[newest] : nullptr;
    if (n && n->key == key && n->serial == serial && SameSpots(n->spots, eyes)) {
        ++stats_.same;
    } else if (ring.drawing >= 0) {
        ++stats_.busy;   // drawn on a later tick, from what's current then
    } else {
        int i = 0;
        while (i == ring.shown || i == ring.before) ++i;
        Output &o = ring.out[i];
        if (!o.bo && !MakeOutput(o, 2 * w, h)) return nullptr;
        const GLuint tex = Texture(key, src);
        if (!tex) return nullptr;
        const bool partial = o.drawn && o.key == key && o.serial == serial;
        Draw(o, tex, w, h, eyes, partial);
        o.fence = pCreateSync(EGLDisplay(dpy_), EGL_SYNC_FENCE_KHR, nullptr);
        glFlush();
        if (!o.fence) glFinish();   // no fence: wait here, as before
        o.key = key, o.serial = serial, o.drawn = true;
        for (int e = 0; e < 2; ++e) o.spots[e] = eyes[e];
        ring.drawing = i;
        ++stats_.draws, stats_.partial += partial;
    }
    // Nothing of ours to show yet: wait for this one rather than show none.
    if (ring.shown < 0 && ring.drawing >= 0) {
        ++stats_.waits;
        if (Passed(ring.out[ring.drawing], 50'000'000)) promote();
    }
    lastMs_ = (SteadyNs() - t0) / 1e6;
    stats_.cpuMs += lastMs_, stats_.worstMs = std::max(stats_.worstMs, lastMs_);
    return ring.shown >= 0 ? &ring.out[ring.shown] : nullptr;
}

}  // namespace handcut
