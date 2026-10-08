// The Frame side of a remote display, shared by the spikes (docs/remote-displays.md): the
// iris hardware decoder (V4L2 stateful API, /dev/video-dec0) exporting its pictures as
// dmabufs, and the GPU pass that turns them into RGBA for SteamVR. Header-only; each spike
// is one translation unit.
#pragma once

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <gbm.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include <openvr.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

namespace {

// The decoder's RGBA outputs: AB24 is V4L2_PIX_FMT_RGBA32 (bytes R, G, B, A), QC24 the
// same in UBWC (Qualcomm's own fourcc, no name in videodev2.h).
constexpr uint32_t kAB24 = v4l2_fourcc('A', 'B', '2', '4');
constexpr uint32_t kQC24 = v4l2_fourcc('Q', 'C', '2', '4');

bool IsRgb(uint32_t fourcc) { return fourcc == kAB24 || fourcc == kQC24; }

volatile sig_atomic_t g_stop = 0;
void Stop(int) { g_stop = 1; }

int64_t MonoNs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
}

int64_t CpuNs() {
    rusage r;
    getrusage(RUSAGE_SELF, &r);
    return (int64_t(r.ru_utime.tv_sec) + r.ru_stime.tv_sec) * 1'000'000'000 +
           (int64_t(r.ru_utime.tv_usec) + r.ru_stime.tv_usec) * 1000;
}

int Xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }

std::string Fourcc(uint32_t f) {
    return {char(f & 0xff), char(f >> 8 & 0xff), char(f >> 16 & 0xff), char(f >> 24 & 0xff)};
}

struct Layout {
    uint32_t offset[2], stride[2], size;
};

// Where the decoder puts its planes in one buffer, after msm_media_info.h (VENUS_*),
// which the iris driver sizes its buffers with. Q08C is NV12 in UBWC: per plane, a
// metadata block, then the compressed pixels, each 4 KiB aligned. QC24 is one RGBA plane
// the same way (16x4 pixel tiles); the importer finds the pixels after the metadata itself.
Layout PlaneLayout(uint32_t fourcc, uint32_t w, uint32_t h) {
    Layout l{};
    if (fourcc == kAB24) {
        l.stride[0] = l.stride[1] = Align(w * 4, 128);
        l.size = Align(l.stride[0] * Align(h, 32), 4096);
        return l;
    }
    if (fourcc == kQC24) {
        l.stride[0] = l.stride[1] = Align(w * 4, 256);
        const uint32_t meta = Align(Align((w + 15) / 16, 64) * Align((h + 3) / 4, 16), 4096);
        l.size = meta + Align(l.stride[0] * Align(h, 16), 4096);
        return l;
    }
    const uint32_t stride = Align(w, 128);
    if (fourcc == V4L2_PIX_FMT_QC08C) {
        const uint32_t yMeta = Align(Align((w + 31) / 32, 64) * Align((h + 7) / 8, 16), 4096);
        const uint32_t y = Align(stride * Align(h, 32), 4096);
        const uint32_t uvMeta = Align(Align(((w + 1) / 2 + 15) / 16, 64) * Align(((h + 1) / 2 + 7) / 8, 16), 4096);
        const uint32_t uv = Align(stride * Align((h + 1) / 2, 32), 4096);
        l.offset[1] = yMeta + y;
        l.size = yMeta + y + uvMeta + uv;
    } else {
        l.offset[1] = stride * Align(h, 32);
        l.size = Align(l.offset[1] + stride * Align((h + 1) / 2, 16), 4096);
    }
    l.offset[0] = 0;
    l.stride[0] = l.stride[1] = stride;
    return l;
}

vr::SharedTextureHandle_t Import(uint32_t w, uint32_t h, uint32_t format, uint64_t modifier, int planes,
                                 const int *fd, const uint32_t *offset, const uint32_t *stride) {
    vr::DmabufAttributes_t a{};
    a.unWidth = w, a.unHeight = h;
    a.unDepth = a.unMipLevels = a.unArrayLayers = a.unSampleCount = 1;
    a.unFormat = format;
    a.ulModifier = modifier;
    a.unPlaneCount = uint32_t(planes);
    for (int i = 0; i < planes; ++i) {
        a.plane[i].unOffset = offset[i];
        a.plane[i].unStride = stride[i];
        a.plane[i].nFd = fd[i];
    }
    vr::SharedTextureHandle_t t = 0;
    if (!vr::VRIPCResourceManager()->ImportDmabuf(vr::VRApplication_Overlay, &a, &t)) return 0;
    return t;
}


struct Decoder {
    int fd = -1;
    uint32_t codec = V4L2_PIX_FMT_HEVC;  // what goes in: HEVC or H264
    uint32_t fourcc = V4L2_PIX_FMT_QC08C;
    int keep = 2;
    struct Out {
        void *map = nullptr;
        size_t len = 0;
        bool busy = false;
    };
    std::vector<Out> out;
    struct Cap {
        int dmabuf = -1;
        vr::SharedTextureHandle_t tex = 0;
    };
    std::vector<Cap> cap;
    std::deque<int> shown;  // capture buffers SteamVR may still read, oldest first
    uint32_t planes = 1, width = 0, height = 0;
    bool toSteamVR = true;  // false with --convert: the converter imports the buffers instead
    Layout layout{};
    uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
    bool capturing = false, drained = false;

    bool Open(const char *dev, int count) {
        fd = open(dev, O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) return std::perror(dev), false;
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        f.fmt.pix_mp.pixelformat = codec;
        f.fmt.pix_mp.width = 1920, f.fmt.pix_mp.height = 1080;  // replaced by the stream's own size
        f.fmt.pix_mp.num_planes = 1;
        f.fmt.pix_mp.plane_fmt[0].sizeimage = 4 << 20;
        if (Xioctl(fd, VIDIOC_S_FMT, &f) < 0) return std::perror("S_FMT output"), false;
        // Ask for the capture format now too; some drivers only take it before streaming.
        v4l2_format c{};
        c.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (Xioctl(fd, VIDIOC_G_FMT, &c) == 0) {
            c.fmt.pix_mp.pixelformat = fourcc;
            if (Xioctl(fd, VIDIOC_S_FMT, &c) < 0) std::perror("S_FMT capture (early)");
        }
        v4l2_event_subscription sub{};
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        if (Xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0) std::perror("subscribe source change");
        sub.type = V4L2_EVENT_EOS;
        Xioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
        v4l2_requestbuffers rb{};
        rb.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, rb.memory = V4L2_MEMORY_MMAP, rb.count = uint32_t(count);
        if (Xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) return std::perror("REQBUFS output"), false;
        out.resize(rb.count);
        for (uint32_t i = 0; i < rb.count; ++i) {
            v4l2_plane p[VIDEO_MAX_PLANES]{};
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, b.memory = V4L2_MEMORY_MMAP, b.index = i;
            b.m.planes = p, b.length = VIDEO_MAX_PLANES;
            if (Xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) return std::perror("QUERYBUF output"), false;
            out[i].len = p[0].length;
            out[i].map = mmap(nullptr, p[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p[0].m.mem_offset);
            if (out[i].map == MAP_FAILED) return std::perror("mmap output"), false;
        }
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (Xioctl(fd, VIDIOC_STREAMON, &type) < 0) return std::perror("STREAMON output"), false;
        std::printf("decoder: %u bitstream buffers of %zu KiB\n", rb.count, out[0].len / 1024);
        return true;
    }

    // Queues one access unit; false when every bitstream buffer is still with the decoder.
    bool Feed(const uint8_t *data, size_t len, uint64_t frame) {
        Reclaim();
        auto it = std::find_if(out.begin(), out.end(), [](const Out &o) { return !o.busy; });
        if (it == out.end()) return false;
        if (len > it->len) {
            std::fprintf(stderr, "access unit of %zu bytes doesn't fit\n", len);
            return true;
        }
        std::memcpy(it->map, data, len);
        v4l2_plane p{};
        p.bytesused = uint32_t(len);
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, b.memory = V4L2_MEMORY_MMAP;
        b.index = uint32_t(it - out.begin());
        b.m.planes = &p, b.length = 1;
        b.timestamp.tv_sec = time_t(frame / 1'000'000), b.timestamp.tv_usec = suseconds_t(frame % 1'000'000);
        if (Xioctl(fd, VIDIOC_QBUF, &b) < 0) return std::perror("QBUF output"), true;
        it->busy = true;
        return true;
    }

    void Reclaim() {
        for (;;) {
            v4l2_plane p[VIDEO_MAX_PLANES]{};
            v4l2_buffer b{};
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, b.memory = V4L2_MEMORY_MMAP;
            b.m.planes = p, b.length = VIDEO_MAX_PLANES;
            if (Xioctl(fd, VIDIOC_DQBUF, &b) < 0) break;
            if (b.index < out.size()) out[b.index].busy = false;
        }
    }

    void Drain() {
        v4l2_decoder_cmd cmd{};
        cmd.cmd = V4L2_DEC_CMD_STOP;
        if (Xioctl(fd, VIDIOC_DECODER_CMD, &cmd) < 0) std::perror("DECODER_CMD stop");
    }

    // Returns true when the capture side was (re)configured.
    bool Events() {
        bool changed = false;
        for (;;) {
            v4l2_event e{};
            if (Xioctl(fd, VIDIOC_DQEVENT, &e) < 0) break;
            if (e.type == V4L2_EVENT_SOURCE_CHANGE) changed = true;
            if (e.type == V4L2_EVENT_EOS) drained = true;
        }
        if (changed && !capturing) return SetUpCapture();
        if (changed) std::printf("decoder: another source change (ignored)\n");
        return false;
    }

    bool SetUpCapture() {
        v4l2_format f{};
        f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (Xioctl(fd, VIDIOC_G_FMT, &f) < 0) return std::perror("G_FMT capture"), false;
        f.fmt.pix_mp.pixelformat = fourcc;
        if (Xioctl(fd, VIDIOC_S_FMT, &f) < 0) return std::perror("S_FMT capture"), false;
        if (Xioctl(fd, VIDIOC_G_FMT, &f) < 0) return std::perror("G_FMT capture"), false;
        const auto &pm = f.fmt.pix_mp;
        planes = pm.num_planes;
        width = pm.width, height = pm.height;
        v4l2_selection sel{};
        sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, sel.target = V4L2_SEL_TGT_COMPOSE;
        if (Xioctl(fd, VIDIOC_G_SELECTION, &sel) < 0) {
            sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (Xioctl(fd, VIDIOC_G_SELECTION, &sel) < 0) sel.r = {0, 0, pm.width, pm.height};
        }
        width = sel.r.width, height = sel.r.height;
        std::printf("decoder: capture %s, coded %ux%u, visible %ux%u at %d,%d, %u plane(s):", Fourcc(pm.pixelformat).c_str(),
                    pm.width, pm.height, width, height, sel.r.left, sel.r.top, planes);
        for (uint32_t i = 0; i < planes; ++i)
            std::printf(" [%u bytes/line, %u bytes]", pm.plane_fmt[i].bytesperline, pm.plane_fmt[i].sizeimage);
        const Layout l = PlaneLayout(fourcc, pm.width, pm.height);
        if (IsRgb(fourcc))
            std::printf("\ndecoder: computed layout: stride %u, %u bytes\n", l.stride[0], l.size);
        else
            std::printf("\ndecoder: computed layout: Y at %u, UV at %u, stride %u, %u bytes\n", l.offset[0], l.offset[1],
                        l.stride[0], l.size);
        if (pm.pixelformat != fourcc) return std::fprintf(stderr, "the decoder won't produce %s\n", Fourcc(fourcc).c_str()), false;

        v4l2_control min{};
        min.id = V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
        const int need = Xioctl(fd, VIDIOC_G_CTRL, &min) == 0 ? min.value : 4;
        v4l2_requestbuffers rb{};
        rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, rb.memory = V4L2_MEMORY_MMAP;
        rb.count = uint32_t(need + keep + 2);
        if (Xioctl(fd, VIDIOC_REQBUFS, &rb) < 0) return std::perror("REQBUFS capture"), false;
        std::printf("decoder: needs %d capture buffers, has %u\n", need, rb.count);
        cap.resize(rb.count);
        const bool rgb = IsRgb(fourcc);
        modifier = fourcc == V4L2_PIX_FMT_QC08C || fourcc == kQC24 ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
        layout = l;
        const uint32_t format = rgb ? DRM_FORMAT_ABGR8888 : DRM_FORMAT_NV12;
        for (uint32_t i = 0; i < rb.count; ++i) {
            v4l2_exportbuffer e{};
            e.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, e.index = i, e.plane = 0, e.flags = O_RDONLY | O_CLOEXEC;
            if (Xioctl(fd, VIDIOC_EXPBUF, &e) < 0) return std::perror("EXPBUF"), false;
            cap[i].dmabuf = e.fd;
            const int fds[2] = {e.fd, e.fd};
            uint32_t stride[2] = {l.stride[0], l.stride[1]};
            // For RGBA the driver reports bytesperline in pixels, so only NV12 takes it.
            if (!rgb && planes == 1 && pm.plane_fmt[0].bytesperline) stride[0] = stride[1] = pm.plane_fmt[0].bytesperline;
            layout.stride[0] = stride[0], layout.stride[1] = stride[1];
            if (!toSteamVR) continue;
            cap[i].tex = Import(width, height, format, modifier, rgb ? 1 : 2, fds, l.offset, stride);
            if (!cap[i].tex) {
                std::fprintf(stderr, "SteamVR can't import capture buffer %u (%s, modifier 0x%llx)\n", i,
                             rgb ? "ABGR8888" : "NV12", (unsigned long long)modifier);
                return false;
            }
        }
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (Xioctl(fd, VIDIOC_STREAMON, &type) < 0) return std::perror("STREAMON capture"), false;
        for (uint32_t i = 0; i < rb.count; ++i) Requeue(int(i));
        capturing = true;
        return true;
    }

    void Requeue(int i) {
        v4l2_plane p[VIDEO_MAX_PLANES]{};
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.memory = V4L2_MEMORY_MMAP, b.index = uint32_t(i);
        b.m.planes = p, b.length = planes;
        if (Xioctl(fd, VIDIOC_QBUF, &b) < 0) std::perror("QBUF capture");
    }

    // The next decoded buffer: its index and frame number, or -1.
    int Decoded(uint64_t *frame, bool *last) {
        v4l2_plane p[VIDEO_MAX_PLANES]{};
        v4l2_buffer b{};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = p, b.length = planes;
        if (!capturing || Xioctl(fd, VIDIOC_DQBUF, &b) < 0) return -1;
        *frame = uint64_t(b.timestamp.tv_sec) * 1'000'000 + uint64_t(b.timestamp.tv_usec);
        *last = b.flags & V4L2_BUF_FLAG_LAST;
        if (p[0].bytesused == 0) {  // an empty LAST buffer after a drain
            Requeue(int(b.index));
            return -1;
        }
        if (++decoded == 30) Dump(int(b.index), p[0].bytesused);
        return int(b.index);
    }

    // With FT_DECTEST_DUMP=PATH, writes the 30th decoded buffer there as the decoder left it.
    int decoded = 0;
    void Dump(int i, uint32_t used) {
        const char *path = std::getenv("FT_DECTEST_DUMP");
        if (!path) return;
        const off_t len = lseek(cap[i].dmabuf, 0, SEEK_END);
        void *m = mmap(nullptr, size_t(len), PROT_READ, MAP_SHARED, cap[i].dmabuf, 0);
        if (m == MAP_FAILED) return std::perror("mmap capture"), void();
        dma_buf_sync sync{DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
        Xioctl(cap[i].dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
        FILE *f = std::fopen(path, "wb");
        if (f) std::fwrite(m, 1, size_t(len), f), std::fclose(f);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        Xioctl(cap[i].dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
        munmap(m, size_t(len));
        std::printf("decoder: dumped buffer %d (%lld bytes, %u used) to %s\n", i, (long long)len, used, path);
    }

    // Hands buffer i to SteamVR and gives back the oldest one beyond `keep`.
    void Shown(int i) {
        shown.push_back(i);
        while (int(shown.size()) > keep) Requeue(shown.front()), shown.pop_front();
    }

    void Close() {
        if (fd < 0) return;
        for (int type : {V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE})
            Xioctl(fd, VIDIOC_STREAMOFF, &type);
        for (auto &c : cap) {
            if (c.tex) vr::VRIPCResourceManager()->UnrefResource(c.tex);
            if (c.dmabuf >= 0) close(c.dmabuf);
        }
        for (auto &o : out)
            if (o.map && o.map != MAP_FAILED) munmap(o.map, o.len);
        close(fd);
        fd = -1;
    }
};

// A level overlay facing the head, `up` metres above eye height minus 0.25 m.
[[maybe_unused]] vr::HmdMatrix34_t Ahead(const vr::HmdMatrix34_t &head, double distance, double up) {
    const double yaw = std::atan2(head.m[0][2], head.m[2][2]);
    vr::HmdMatrix34_t m{};
    m.m[0][0] = float(std::cos(yaw)), m.m[0][2] = float(std::sin(yaw));
    m.m[1][1] = 1;
    m.m[2][0] = float(-std::sin(yaw)), m.m[2][2] = float(std::cos(yaw));
    m.m[0][3] = float(head.m[0][3] - std::sin(yaw) * distance);
    m.m[1][3] = float(head.m[1][3] - 0.25 + up);
    m.m[2][3] = float(head.m[2][3] - std::cos(yaw) * distance);
    return m;
}

// --convert: the pass SteamVR doesn't do. Each decoder buffer becomes an external texture
// (Mesa converts NV12 to RGB with the matrix and range given as EGL hints), drawn into one
// of three RGBA buffers SteamVR imported once, like screens/handcut.cpp.
PFNEGLGETPLATFORMDISPLAYEXTPROC pGetPlatformDisplay;
PFNEGLCREATEIMAGEKHRPROC pCreateImage;
PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pImageTargetTexture;
PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC pImageTargetRenderbuffer;

const char *kVertex = R"(
attribute vec2 pos;
varying vec2 uv;
void main() {
    uv = pos;  // memory row 0 is y = 0 on both sides, so no flip
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
})";

const char *kConvert = R"(
#extension GL_OES_EGL_image_external : require
precision highp float;
uniform samplerExternalOES tex;
varying vec2 uv;
void main() { gl_FragColor = vec4(texture2D(tex, uv).rgb, 1.0); })";

GLuint Shader(GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = "";
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        std::fprintf(stderr, "shader: %s\n", log);
    }
    return s;
}

// planes: fd, offset, stride each; one modifier for all.
EGLImageKHR EglImage(EGLDisplay dpy, uint32_t w, uint32_t h, uint32_t format, uint64_t modifier, int planes,
                     const int *fd, const uint32_t *offset, const uint32_t *stride, const EGLint *extra) {
    static const EGLint kFd[2] = {EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT};
    static const EGLint kOff[2] = {EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT};
    static const EGLint kPitch[2] = {EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT};
    static const EGLint kLo[2] = {EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT};
    static const EGLint kHi[2] = {EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT};
    EGLint a[64];
    int n = 0;
    a[n++] = EGL_WIDTH, a[n++] = EGLint(w), a[n++] = EGL_HEIGHT, a[n++] = EGLint(h);
    a[n++] = EGL_LINUX_DRM_FOURCC_EXT, a[n++] = EGLint(format);
    for (int i = 0; i < planes && i < 2; ++i) {
        a[n++] = kFd[i], a[n++] = fd[i], a[n++] = kOff[i], a[n++] = EGLint(offset[i]);
        a[n++] = kPitch[i], a[n++] = EGLint(stride[i]);
        a[n++] = kLo[i], a[n++] = EGLint(modifier & 0xffffffff);
        a[n++] = kHi[i], a[n++] = EGLint(modifier >> 32);
    }
    for (; extra && *extra != EGL_NONE; ++extra) a[n++] = *extra;
    a[n++] = EGL_NONE;
    return pCreateImage(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, a);
}

struct Converter {
    EGLint matrix = EGL_ITU_REC709_EXT, range = EGL_YUV_NARROW_RANGE_EXT;
    EGLDisplay dpy = EGL_NO_DISPLAY;
    gbm_device *gbm = nullptr;
    GLuint prog = 0, vbo = 0;
    std::vector<GLuint> in;  // one external texture per decoder buffer
    struct Out {
        gbm_bo *bo = nullptr;
        GLuint fbo = 0;
        vr::SharedTextureHandle_t tex = 0;
        int fd = -1;  // the dmabuf, for a process that shows it (ft-stream hands it to ft-screens)
        uint32_t offset = 0, stride = 0;
        uint64_t modifier = 0;
    } out[3];
    int next = 0;
    uint32_t width = 0, height = 0;
    double msSum = 0, msMax = 0;
    bool toSteamVR = true;  // false: no SteamVR import here (ft-stream, or a measurement without the headset)
    // Without SteamVR: the modifiers to allocate the ring with (ft-screens asks SteamVR which
    // it imports); empty is the UBWC one SteamVR takes on the Frame.
    std::vector<uint64_t> modifiers;

    bool Init() {
        const int drm = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        gbm = drm >= 0 ? gbm_create_device(drm) : nullptr;
        pGetPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        pCreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        pImageTargetTexture = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        pImageTargetRenderbuffer = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
            eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
        if (!gbm || !pGetPlatformDisplay || !pCreateImage || !pImageTargetTexture || !pImageTargetRenderbuffer)
            return std::fprintf(stderr, "convert: GBM or EGL extensions missing\n"), false;
        dpy = pGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) return std::fprintf(stderr, "convert: no EGL display\n"), false;
        eglBindAPI(EGL_OPENGL_ES_API);
        const EGLint attrs[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
        EGLContext ctx = eglCreateContext(dpy, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);
        if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx))
            return std::fprintf(stderr, "convert: no surfaceless GLES context\n"), false;
        prog = glCreateProgram();
        glAttachShader(prog, Shader(GL_VERTEX_SHADER, kVertex));
        glAttachShader(prog, Shader(GL_FRAGMENT_SHADER, kConvert));
        glBindAttribLocation(prog, 0, "pos");
        glLinkProgram(prog);
        GLint ok = 0;
        glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        if (!ok) return std::fprintf(stderr, "convert: program doesn't link\n"), false;
        const float quad[] = {0, 0, 1, 0, 0, 1, 1, 1};
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof quad, quad, GL_STATIC_DRAW);
        return true;
    }

    // Frees what Setup made, for a new one (the stream's size changed).
    void Reset() {
        if (!in.empty()) glDeleteTextures(GLsizei(in.size()), in.data());
        in.clear();
        for (Out &o : out) {
            if (o.fbo) glDeleteFramebuffers(1, &o.fbo);
            if (o.fd >= 0) close(o.fd);
            if (o.bo) gbm_bo_destroy(o.bo);
            o = Out{};
        }
        next = 0;
    }

    // Imports every decoder buffer, and makes the RGBA ring at the visible size.
    bool Setup(const Decoder &dec) {
        Reset();
        width = dec.width, height = dec.height;
        const EGLint hints[] = {EGL_YUV_COLOR_SPACE_HINT_EXT, matrix, EGL_SAMPLE_RANGE_HINT_EXT, range, EGL_NONE};
        for (const auto &c : dec.cap) {
            const int fds[2] = {c.dmabuf, c.dmabuf};
            EGLImageKHR image = EglImage(dpy, width, height, DRM_FORMAT_NV12, dec.modifier, 2, fds, dec.layout.offset,
                                         dec.layout.stride, hints);
            if (image == EGL_NO_IMAGE_KHR) return std::fprintf(stderr, "convert: EGL can't import a decoder buffer (0x%x)\n", eglGetError()), false;
            GLuint t;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, t);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            pImageTargetTexture(GL_TEXTURE_EXTERNAL_OES, image);
            in.push_back(t);
        }
        uint64_t mods[64];
        uint32_t nmods = 64;
        if (!toSteamVR) {
            nmods = 0;
            for (uint64_t m : modifiers)
                if (nmods < 64) mods[nmods++] = m;
            if (!nmods) mods[0] = DRM_FORMAT_MOD_QCOM_COMPRESSED, nmods = 1;
        } else if (!vr::VRIPCResourceManager()->GetDmabufModifiers(vr::VRApplication_Overlay, DRM_FORMAT_ABGR8888, &nmods, mods)) {
            nmods = 0;
        }
        for (Out &o : out) {
            o.bo = nmods ? gbm_bo_create_with_modifiers2(gbm, width, height, GBM_FORMAT_ABGR8888, mods, nmods, GBM_BO_USE_RENDERING)
                         : gbm_bo_create(gbm, width, height, GBM_FORMAT_ABGR8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
            if (!o.bo) return std::fprintf(stderr, "convert: can't allocate the RGBA ring\n"), false;
            const int fd = gbm_bo_get_fd(o.bo);
            const uint32_t offset = gbm_bo_get_offset(o.bo, 0), stride = gbm_bo_get_stride(o.bo);
            const uint64_t modifier = nmods ? gbm_bo_get_modifier(o.bo) : DRM_FORMAT_MOD_LINEAR;
            o.fd = fd, o.offset = offset, o.stride = stride, o.modifier = modifier;
            EGLImageKHR image = EglImage(dpy, width, height, DRM_FORMAT_ABGR8888, modifier, 1, &fd, &offset, &stride, nullptr);
            GLuint rb;
            glGenRenderbuffers(1, &rb);
            glBindRenderbuffer(GL_RENDERBUFFER, rb);
            pImageTargetRenderbuffer(GL_RENDERBUFFER, image);
            glGenFramebuffers(1, &o.fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
                return std::fprintf(stderr, "convert: output framebuffer incomplete\n"), false;
            if (toSteamVR) o.tex = Import(width, height, DRM_FORMAT_ABGR8888, modifier, 1, &fd, &offset, &stride);
            if (toSteamVR && !o.tex) return std::fprintf(stderr, "convert: SteamVR can't import the RGBA ring\n"), false;
            if (&o == out) std::printf("convert: RGBA ring %ux%u, modifier 0x%llx, stride %u\n", width, height,
                                       (unsigned long long)modifier, stride);
        }
        return true;
    }

    // Draws decoder buffer i into the next ring buffer; returns it once the GPU is done.
    vr::SharedTextureHandle_t Convert(int i) {
        const int at = next;
        next = (next + 1) % 3;
        ConvertInto(i, at);
        return out[at].tex;
    }

    // Draws decoder buffer i into ring buffer `at` (one its viewer has let go of), and
    // returns once the GPU is done.
    void ConvertInto(int i, int at) {
        const int64_t t0 = MonoNs();
        Out &o = out[at];
        glBindFramebuffer(GL_FRAMEBUFFER, o.fbo);
        glViewport(0, 0, GLsizei(width), GLsizei(height));
        glUseProgram(prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, in[size_t(i)]);
        glUniform1i(glGetUniformLocation(prog, "tex"), 0);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glFinish();  // SteamVR reads it from another process and GPU queue
        const double ms = (MonoNs() - t0) / 1e6;
        msSum += ms, msMax = std::max(msMax, ms);
    }
};

}  // namespace
