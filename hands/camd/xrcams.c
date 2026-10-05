/*
 * xrcams - find the headset cameras and the DMA-BUF queues XRService feeds them.
 *
 * Adapted from framecap.c in FrameEyeCameraFeed (vendor/FrameEyeCameraFeed),
 * MIT License, Copyright (c) 2026 Curtis English. See LICENSE.FrameEyeCameraFeed.
 *
 * Everything is discovered rather than hardcoded:
 *   - XRService is found by scanning /proc for its cmdline.
 *   - The V4L2 nodes and sensor subdevs it holds open come from /proc/<pid>/fd.
 *   - Each node's geometry comes from VIDIOC_G_FMT on our own handle.
 *   - Each node is traced back to its sensor through MEDIA_IOC_G_TOPOLOGY.
 *   - Buffers are split into runs by allocation order, and each run is bound to
 *     its camera by VIDIOC_QUERYBUF on the camera's node, which names the
 *     descriptor XRService queued at each index. Where that fails, the sensor
 *     subdev opened just before the run decides (XRService opens a sensor's
 *     subdev, then allocates its buffers), which isn't always right.
 */

#define _GNU_SOURCE

#include "xrcams.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <linux/media.h>

#ifndef MEDIA_ENT_F_CAM_SENSOR
#define MEDIA_ENT_F_CAM_SENSOR 0x00020001
#endif

#define MAX_FDENTS 4096
#define MAX_TOPOS     8

enum fdkind { FD_DMABUF, FD_SUBDEV_SENSOR, FD_VIDEO };

typedef struct {
    int           xfd;
    enum fdkind   kind;
    size_t        size;
    unsigned long ino;
    char          sensor[XR_SENSOR_LEN];
    char          path[64];
} fdent_t;

typedef struct {
    struct media_v2_entity    *ents;
    struct media_v2_interface *intfs;
    struct media_v2_pad       *pads;
    struct media_v2_link      *links;
    __u32 nents, nintfs, npads, nlinks;
} topo_t;

static fdent_t fdents[MAX_FDENTS];
static int     nfdents;
static topo_t  topos[MAX_TOPOS];
static int     ntopos;

static void set_err(char *err, size_t n, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

void xr_slugify(const char *in, char *out, size_t n)
{
    size_t i = 0;

    for (; in[i] && i + 1 < n; i++)
        out[i] = (in[i] == ' ' || in[i] == '/') ? '_' : in[i];

    out[i] = 0;
}

/* --------------------------------------------------- media graph handling */

static void topo_free_all(void)
{
    for (int i = 0; i < ntopos; i++) {
        free(topos[i].ents);
        free(topos[i].intfs);
        free(topos[i].pads);
        free(topos[i].links);
    }

    ntopos = 0;
}

static void topo_load_all(void)
{
    for (int mi = 0; mi < MAX_TOPOS; mi++) {

        char mpath[32];
        snprintf(mpath, sizeof(mpath), "/dev/media%d", mi);

        int mfd = open(mpath, O_RDWR | O_CLOEXEC);

        if (mfd < 0)
            continue;

        struct media_v2_topology t;
        memset(&t, 0, sizeof(t));

        if (ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &t) < 0) {
            close(mfd);
            continue;
        }

        topo_t *o = &topos[ntopos];
        memset(o, 0, sizeof(*o));

        o->nents  = t.num_entities;
        o->nintfs = t.num_interfaces;
        o->npads  = t.num_pads;
        o->nlinks = t.num_links;

        o->ents  = calloc(o->nents  ? o->nents  : 1, sizeof(*o->ents));
        o->intfs = calloc(o->nintfs ? o->nintfs : 1, sizeof(*o->intfs));
        o->pads  = calloc(o->npads  ? o->npads  : 1, sizeof(*o->pads));
        o->links = calloc(o->nlinks ? o->nlinks : 1, sizeof(*o->links));

        t.ptr_entities   = (__u64)(uintptr_t)o->ents;
        t.ptr_interfaces = (__u64)(uintptr_t)o->intfs;
        t.ptr_pads       = (__u64)(uintptr_t)o->pads;
        t.ptr_links      = (__u64)(uintptr_t)o->links;

        bool ok = o->ents && o->intfs && o->pads && o->links &&
                  ioctl(mfd, MEDIA_IOC_G_TOPOLOGY, &t) == 0;
        close(mfd);

        if (!ok) {
            free(o->ents); free(o->intfs); free(o->pads); free(o->links);
            continue;
        }

        ntopos++;
    }
}

static struct media_v2_entity *topo_entity(topo_t *t, __u32 id)
{
    for (__u32 i = 0; i < t->nents; i++)
        if (t->ents[i].id == id)
            return &t->ents[i];

    return NULL;
}

static struct media_v2_pad *topo_pad(topo_t *t, __u32 id)
{
    for (__u32 i = 0; i < t->npads; i++)
        if (t->pads[i].id == id)
            return &t->pads[i];

    return NULL;
}

static __u32 topo_entity_for_devnode(topo_t *t, dev_t rdev)
{
    __u32 intf_id = 0;

    for (__u32 i = 0; i < t->nintfs; i++)
        if (t->intfs[i].devnode.major == major(rdev) &&
            t->intfs[i].devnode.minor == minor(rdev)) {
            intf_id = t->intfs[i].id;
            break;
        }

    if (!intf_id)
        return 0;

    for (__u32 i = 0; i < t->nlinks; i++)
        if ((t->links[i].flags & MEDIA_LNK_FL_LINK_TYPE) == MEDIA_LNK_FL_INTERFACE_LINK &&
            t->links[i].source_id == intf_id)
            return t->links[i].sink_id;

    return 0;
}

/*
 * Walk upstream across enabled data links until a sensor is reached. A CSIPHY
 * carries two sensors on separate (sink, source) pad pairs, so re-enter on the
 * sink pad paired with the source pad we left through.
 */
static bool topo_walk_to_sensor(topo_t *t, __u32 ent_id, char *out, size_t outn)
{
    int exit_pad_index = -1;

    for (int hop = 0; hop < 32 && ent_id; hop++) {

        struct media_v2_entity *e = topo_entity(t, ent_id);

        if (!e)
            return false;

        if (e->function == MEDIA_ENT_F_CAM_SENSOR) {
            snprintf(out, outn, "%s", e->name);
            return true;
        }

        __u32 first_sink = 0, paired = 0;
        int   nsinks     = 0;

        for (__u32 p = 0; p < t->npads; p++) {

            if (t->pads[p].entity_id != ent_id || !(t->pads[p].flags & MEDIA_PAD_FL_SINK))
                continue;

            nsinks++;

            if (!first_sink)
                first_sink = t->pads[p].id;

            if (exit_pad_index >= 1 && (int)t->pads[p].index == exit_pad_index - 1)
                paired = t->pads[p].id;
        }

        __u32 sink_pad = (nsinks == 1) ? first_sink : (paired ? paired : first_sink);

        if (!sink_pad)
            return false;

        __u32 src_pad = 0;

        for (__u32 i = 0; i < t->nlinks; i++) {

            if ((t->links[i].flags & MEDIA_LNK_FL_LINK_TYPE) != MEDIA_LNK_FL_DATA_LINK)
                continue;

            if (!(t->links[i].flags & MEDIA_LNK_FL_ENABLED))
                continue;

            if (t->links[i].sink_id == sink_pad) {
                src_pad = t->links[i].source_id;
                break;
            }
        }

        struct media_v2_pad *sp = src_pad ? topo_pad(t, src_pad) : NULL;

        if (!sp)
            return false;

        ent_id         = sp->entity_id;
        exit_pad_index = (int)sp->index;
    }

    return false;
}

static bool sensor_for_video(dev_t rdev, char *out, size_t outn)
{
    for (int i = 0; i < ntopos; i++) {

        __u32 ent = topo_entity_for_devnode(&topos[i], rdev);

        if (ent && topo_walk_to_sensor(&topos[i], ent, out, outn))
            return true;
    }

    return false;
}

static bool sensor_for_subdev(dev_t rdev, char *out, size_t outn)
{
    for (int i = 0; i < ntopos; i++) {

        __u32 id = topo_entity_for_devnode(&topos[i], rdev);
        struct media_v2_entity *e = id ? topo_entity(&topos[i], id) : NULL;

        if (e && e->function == MEDIA_ENT_F_CAM_SENSOR) {
            snprintf(out, outn, "%s", e->name);
            return true;
        }
    }

    return false;
}

static const char *role_for_sensor(const char *sensor)
{
    if (strstr(sensor, "og01a1b"))
        return "tracking";      /* 1056x1024 side fisheye      */

    if (strstr(sensor, "og0ve10"))
        return "tracking";      /* 640x480 upper               */

    if (strstr(sensor, "imx616"))
        return "passthrough";   /* 2464x2464 Arcturus color    */

    return "unknown";
}

/* ------------------------------------------------- XRService / proc scan */

static pid_t find_process(const char *needle)
{
    DIR *d = opendir("/proc");

    if (!d)
        return 0;

    struct dirent *e;
    pid_t found = 0;

    while ((e = readdir(d))) {

        if (e->d_name[0] < '0' || e->d_name[0] > '9')
            continue;

        char path[288];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);

        FILE *f = fopen(path, "rb");

        if (!f)
            continue;

        char buf[512] = {0};
        size_t got = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);

        if (got == 0)
            continue;

        const char *base = strrchr(buf, '/');
        base = base ? base + 1 : buf;

        if (strstr(base, needle)) {
            found = (pid_t)atoi(e->d_name);
            break;
        }
    }

    closedir(d);

    return found;
}

static bool read_dmabuf_size(pid_t pid, int fd, size_t *size, unsigned long *ino)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fdinfo/%d", pid, fd);

    FILE *f = fopen(path, "r");

    if (!f)
        return false;

    bool have = false;
    char line[256];

    *ino = 0;

    while (fgets(line, sizeof(line), f)) {

        unsigned long long v;

        if (sscanf(line, "size: %llu", &v) == 1) {
            *size = (size_t)v;
            have  = true;
        } else if (sscanf(line, "ino: %llu", &v) == 1) {
            *ino = (unsigned long)v;
        }
    }

    fclose(f);

    return have;
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

static bool scan_xr_fds(pid_t pid, char *err, size_t errn)
{
    char dirpath[64];
    snprintf(dirpath, sizeof(dirpath), "/proc/%d/fd", pid);

    DIR *d = opendir(dirpath);

    if (!d) {
        set_err(err, errn, "opendir(%s): %s (are you root?)", dirpath, strerror(errno));
        return false;
    }

    static int fds[8192];
    int nfds = 0;
    struct dirent *e;

    while ((e = readdir(d)) && nfds < (int)(sizeof(fds) / sizeof(fds[0])))
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9')
            fds[nfds++] = atoi(e->d_name);

    closedir(d);

    qsort(fds, nfds, sizeof(int), cmp_int);

    nfdents = 0;

    for (int i = 0; i < nfds && nfdents < MAX_FDENTS; i++) {

        char link[64], target[256];
        snprintf(link, sizeof(link), "/proc/%d/fd/%d", pid, fds[i]);

        ssize_t n = readlink(link, target, sizeof(target) - 1);

        if (n < 0)
            continue;

        target[n] = 0;

        fdent_t ent;
        memset(&ent, 0, sizeof(ent));
        ent.xfd = fds[i];

        if (strstr(target, "dmabuf")) {

            if (!read_dmabuf_size(pid, fds[i], &ent.size, &ent.ino))
                continue;

            ent.kind = FD_DMABUF;

        } else if (strncmp(target, "/dev/video", 10) == 0) {

            ent.kind = FD_VIDEO;
            snprintf(ent.path, sizeof(ent.path), "%.63s", target);

        } else if (strncmp(target, "/dev/v4l-subdev", 15) == 0) {

            struct stat st;

            if (stat(target, &st) < 0 || !sensor_for_subdev(st.st_rdev, ent.sensor, sizeof(ent.sensor)))
                continue;

            ent.kind = FD_SUBDEV_SENSOR;

        } else {
            continue;
        }

        fdents[nfdents++] = ent;
    }

    return true;
}

/* ------------------------------------------------------ camera discovery */

/*
 * Which of XRService's buffers each V4L2 index of a camera holds. vb2 lets any handle query a
 * queue's buffers, and for a DMABUF buffer it returns the descriptor its owner last queued it
 * with: that descriptor's number in XRService's fd table, the same numbers scan_xr_fds reads
 * from /proc. XRService queues each index with the same buffer every time.
 */
static void query_buffers(int fd, xr_camera_t *c, bool mplane)
{
    c->nqbuf = 0;

    for (int i = 0; i < XR_MAX_RUNBUFS; i++) {

        struct v4l2_buffer b;
        struct v4l2_plane  planes[VIDEO_MAX_PLANES];

        memset(&b, 0, sizeof(b));
        memset(planes, 0, sizeof(planes));
        b.index = (unsigned)i;
        b.type  = mplane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE;

        if (mplane) {
            b.m.planes = planes;
            b.length   = VIDEO_MAX_PLANES;
        }

        if (ioctl(fd, VIDIOC_QUERYBUF, &b) < 0 || b.memory != V4L2_MEMORY_DMABUF)
            break;      /* EINVAL past the last index */

        c->qbuf_xfd[c->nqbuf++] = mplane ? planes[0].m.fd : b.m.fd;
    }
}

static void probe_cameras(xr_state_t *st)
{
    int seen[64];
    int nseen = 0;

    for (int i = 0; i < nfdents; i++) {

        if (fdents[i].kind != FD_VIDEO)
            continue;

        const char *path = fdents[i].path;
        int         node = atoi(path + 10);
        bool        dup  = false;

        for (int k = 0; k < nseen; k++)
            if (seen[k] == node)
                dup = true;

        if (dup || st->ncameras >= XR_MAX_CAMERAS || nseen >= 64)
            continue;

        seen[nseen++] = node;

        int fd = open(path, O_RDWR | O_CLOEXEC);

        if (fd < 0)
            continue;

        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

        xr_camera_t *c = &st->cameras[st->ncameras];
        memset(c, 0, sizeof(*c));

        if (ioctl(fd, VIDIOC_G_FMT, &fmt) == 0) {

            c->width        = fmt.fmt.pix_mp.width;
            c->height       = fmt.fmt.pix_mp.height;
            c->pixfmt       = fmt.fmt.pix_mp.pixelformat;
            c->nplanes      = fmt.fmt.pix_mp.num_planes;
            c->bytesperline = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;

            for (unsigned p = 0; p < c->nplanes && p < VIDEO_MAX_PLANES; p++)
                c->planesize[p] = fmt.fmt.pix_mp.plane_fmt[p].sizeimage;

        } else {

            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

            if (ioctl(fd, VIDIOC_G_FMT, &fmt) < 0) {
                close(fd);
                continue;
            }

            c->width        = fmt.fmt.pix.width;
            c->height       = fmt.fmt.pix.height;
            c->pixfmt       = fmt.fmt.pix.pixelformat;
            c->nplanes      = 1;
            c->bytesperline = fmt.fmt.pix.bytesperline;
            c->planesize[0] = fmt.fmt.pix.sizeimage;
        }

        query_buffers(fd, c, fmt.type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);

        struct stat sb;

        if (fstat(fd, &sb) == 0) {
            c->minor = minor(sb.st_rdev);
            sensor_for_video(sb.st_rdev, c->sensor, sizeof(c->sensor));
        }

        close(fd);

        if (!c->sensor[0])
            snprintf(c->sensor, sizeof(c->sensor), "unknown");

        c->node = node;
        snprintf(c->path, sizeof(c->path), "%s", path);
        c->role = role_for_sensor(c->sensor);

        st->ncameras++;
    }
}

/*
 * qcom-camss can report bytesperline as the visible width while the VFE
 * writes a larger aligned pitch. sizeimage is right, so derive the pitch.
 * For NV12, plane 0 normally holds the chroma rows after the luma (the side
 * cameras through the ISP: 1056 wide, 1152 bytes a row); if it's too small for
 * that, it holds the luma alone.
 */
unsigned xr_camera_stride(const xr_camera_t *c)
{
    if (!c->height || !c->planesize[0])
        return c->bytesperline ? c->bytesperline : c->width;

    bool yuv = c->pixfmt == V4L2_PIX_FMT_NV12 || c->pixfmt == V4L2_PIX_FMT_NV21;
    unsigned s = (unsigned)((double)c->planesize[0] / ((double)c->height * (yuv ? 1.5 : 1.0)));

    if (s >= c->width && s <= c->width * 4)
        return s;

    s = (unsigned)(c->planesize[0] / c->height);

    if (yuv && s >= c->width && s <= c->width * 4)
        return s;

    return c->bytesperline ? c->bytesperline : c->width;
}

/*
 * The Arcturus color cameras (arcimx616) claim 2464x2464 NV12, but measured on
 * 2026-09-28 their plane 0 holds 10-bit MIPI-packed YUV 4:2:0: 2464 luma rows
 * then 1232 rows of interleaved UV, each row 2464 packed pixels (3080 bytes)
 * padded to a 256-byte pitch (3328). Only the first 1972 pixels of a row carry
 * image; the rest are zero.
 */
#define IMX616_VALID_WIDTH 1972

void xr_camera_layout(const xr_camera_t *c, xr_layout_t *l)
{
    memset(l, 0, sizeof(*l));
    l->height = c->height;

    if (c->pixfmt == V4L2_PIX_FMT_NV12 && strstr(c->sensor, "imx616")) {

        unsigned packed = (c->width * 5 + 3) / 4;

        l->fmt   = XR_FMT_YUV420_10P;
        l->pitch = (packed + 255) & ~255u;
        l->rows  = c->height + c->height / 2;
        l->width = IMX616_VALID_WIDTH < c->width ? IMX616_VALID_WIDTH : c->width;
        return;
    }

    l->pitch = xr_camera_stride(c);
    l->width = c->width < l->pitch ? c->width : l->pitch;

    /*
     * Without the colour module, XRService runs the side cameras through the
     * ISP ("ISP enabled for tracking cameras (main VFE available)" in its log),
     * and they come out NV12. They're mono sensors, so the luma is the image.
     */
    bool yuv = c->pixfmt == V4L2_PIX_FMT_NV12 || c->pixfmt == V4L2_PIX_FMT_NV21;

    if (yuv && c->role && !strcmp(c->role, "tracking")) {
        l->fmt  = XR_FMT_GREY8;
        l->rows = c->height;
        return;
    }

    if (yuv) {
        l->fmt  = XR_FMT_NV12;
        l->rows = c->height + c->height / 2;
    } else {
        l->fmt  = XR_FMT_GREY8;
        l->rows = c->height;
    }
}

const char *xr_fmt_name(xr_fmt_t f)
{
    switch (f) {
    case XR_FMT_GREY8:      return "grey8";
    case XR_FMT_NV12:       return "nv12";
    case XR_FMT_YUV420_10P: return "yuv420_10p";
    }

    return "?";
}

/* ------------------------------------------------------- buffer grouping */

/* The camera whose VIDIOC_QUERYBUF names this XRService descriptor, or NULL. */
static xr_camera_t *qbuf_owner(xr_state_t *st, int xfd)
{
    for (int c = 0; c < st->ncameras; c++)
        for (int k = 0; k < st->cameras[c].nqbuf; k++)
            if (st->cameras[c].qbuf_xfd[k] == xfd)
                return &st->cameras[c];

    return NULL;
}

/*
 * A run can hold two cameras' queues when nothing between them in the fd table ends it (the
 * upper pair, both 640x480). Where VIDIOC_QUERYBUF says so, cut it where the owner changes.
 */
static void split_groups(xr_state_t *st)
{
    for (int i = 0; i < st->ngroups && st->ngroups < XR_MAX_GROUPS; i++) {

        xr_group_t  *g     = &st->groups[i];
        xr_camera_t *first = qbuf_owner(st, g->buf[0].xfd);
        int          cut   = -1;

        for (int b = 1; first && b < g->nbufs && cut < 0; b++) {
            xr_camera_t *o = qbuf_owner(st, g->buf[b].xfd);
            if (o && o != first)
                cut = b;
        }

        if (cut < 0)
            continue;

        xr_group_t *t = &st->groups[st->ngroups++];
        *t = *g;
        t->nbufs = g->nbufs - cut;
        memmove(t->buf, g->buf + cut, (size_t)t->nbufs * sizeof(t->buf[0]));
        g->nbufs = cut;
    }
}

/*
 * XRService allocates one udmabuf per plane, plane 0 then plane 1, a whole
 * queue at a time right after opening the sensor's subdev. Plane 1 matches
 * VIDIOC_G_FMT exactly; plane 0 has slack, so it is matched with >=.
 */
static void build_groups(xr_state_t *st)
{
    char current_sensor[XR_SENSOR_LEN] = "";

    for (int i = 0; i < nfdents; i++) {

        if (fdents[i].kind == FD_SUBDEV_SENSOR) {
            snprintf(current_sensor, sizeof(current_sensor), "%s", fdents[i].sensor);
            continue;
        }

        if (fdents[i].kind != FD_DMABUF)
            continue;

        if (i + 1 >= nfdents || fdents[i + 1].kind != FD_DMABUF)
            continue;

        size_t s0 = fdents[i].size;
        size_t s1 = fdents[i + 1].size;
        bool match = false;

        for (int c = 0; c < st->ncameras; c++) {

            xr_camera_t *cam = &st->cameras[c];

            if (cam->nplanes >= 2 && s1 == cam->planesize[1] && s0 >= cam->planesize[0]) {
                match = true;
                break;
            }
        }

        if (!match)
            continue;

        xr_group_t *g = NULL;

        if (st->ngroups > 0) {

            xr_group_t *last = &st->groups[st->ngroups - 1];

            if (last->planesize[0] == s0 && last->planesize[1] == s1 &&
                !strcmp(last->sensor, current_sensor))
                g = last;
        }

        if (!g) {

            if (st->ngroups >= XR_MAX_GROUPS)
                break;

            g = &st->groups[st->ngroups++];
            memset(g, 0, sizeof(*g));
            g->planesize[0] = s0;
            g->planesize[1] = s1;
            snprintf(g->sensor, sizeof(g->sensor), "%s", current_sensor);
        }

        if (g->nbufs < XR_MAX_RUNBUFS) {
            g->buf[g->nbufs].xfd   = fdents[i].xfd;
            g->buf[g->nbufs].xfd1  = fdents[i + 1].xfd;
            g->buf[g->nbufs].size  = s0;
            g->buf[g->nbufs].size1 = s1;
            g->nbufs++;
        }

        i++;    /* consume the plane 1 descriptor */
    }

    split_groups(st);

    int keep = 0;

    for (int i = 0; i < st->ngroups; i++)
        if (st->groups[i].nbufs >= 4)
            st->groups[keep++] = st->groups[i];

    st->ngroups = keep;

    /*
     * Bind each run to a camera: exactly where a camera's VIDIOC_QUERYBUF names the run's first
     * buffer (query_buffers). The two side cameras have the same format, so for them nothing
     * else is sure.
     */
    for (int i = 0; i < st->ngroups; i++)
        for (int c = 0; c < st->ncameras && !st->groups[i].cam; c++)
            for (int k = 0; k < st->cameras[c].nqbuf; k++)
                if (st->cameras[c].qbuf_xfd[k] == st->groups[i].buf[0].xfd) {
                    st->groups[i].cam   = &st->cameras[c];
                    st->groups[i].exact = true;
                    break;
                }

    /*
     * The rest by the sensor marker and sizes. The sensor marker alone can be wrong: XRService
     * sometimes opens another sensor's subdev (e.g. the idle color camera)
     * between an upper camera's subdev and its buffers, and two upper cameras
     * can resolve to the same sensor name. So a marker match must also fit the
     * camera's plane sizes, and each camera takes at most one run.
     */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < st->ngroups; i++) {

            xr_group_t *g = &st->groups[i];

            for (int c = 0; c < st->ncameras && !g->cam; c++) {

                xr_camera_t *cam = &st->cameras[c];

                if (pass == 0 && (!g->sensor[0] || strcmp(cam->sensor, g->sensor)))
                    continue;

                if (cam->nplanes < 2 || g->planesize[1] != cam->planesize[1] ||
                    g->planesize[0] < cam->planesize[0])
                    continue;

                bool taken = false;

                for (int k = 0; k < st->ngroups; k++)
                    if (k != i && st->groups[k].cam == cam)
                        taken = true;

                if (!taken)
                    g->cam = cam;
            }
        }
}

bool xr_discover(xr_state_t *st, const char *process, char *err, size_t errn)
{
    memset(st, 0, sizeof(*st));

    st->pid = find_process(process);

    if (!st->pid) {
        set_err(err, errn, "%s is not running; start SteamVR on the headset first", process);
        return false;
    }

    topo_load_all();

    bool ok = scan_xr_fds(st->pid, err, errn);

    if (ok) {
        probe_cameras(st);
        build_groups(st);
    }

    topo_free_all();

    return ok;
}

void xr_print(const xr_state_t *st, FILE *f)
{
    fprintf(f, "XRService pid %d\n", st->pid);

    for (int i = 0; i < st->ncameras; i++) {

        const xr_camera_t *c = &st->cameras[i];
        char fcc[5] = {
            (char)(c->pixfmt & 0xff), (char)((c->pixfmt >> 8) & 0xff),
            (char)((c->pixfmt >> 16) & 0xff), (char)((c->pixfmt >> 24) & 0xff), 0
        };

        fprintf(f, "  camera %-12s minor %-3u %-16s %ux%u %s pitch %u planes %zu %zu  role=%s\n",
                c->path, c->minor, c->sensor, c->width, c->height, fcc,
                xr_camera_stride(c), c->planesize[0], c->planesize[1], c->role);
    }

    for (int i = 0; i < st->ngroups; i++) {

        const xr_group_t *g = &st->groups[i];

        fprintf(f, "  queue %d: %d buffers plane0=%zu plane1=%zu fds %d..%d sensor '%s' -> %s%s\n",
                i, g->nbufs, g->planesize[0], g->planesize[1],
                g->buf[0].xfd, g->buf[g->nbufs - 1].xfd1, g->sensor,
                g->cam ? g->cam->path : "(unbound)", g->exact ? " (VIDIOC_QUERYBUF)" : "");
    }
}
