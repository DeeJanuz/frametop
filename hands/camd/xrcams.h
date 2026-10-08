/*
 * xrcams - find the headset cameras and the DMA-BUF queues XRService feeds them.
 *
 * Adapted from framecap.c in FrameEyeCameraFeed (vendor/FrameEyeCameraFeed),
 * MIT License, Copyright (c) 2026 Curtis English. See LICENSE.FrameEyeCameraFeed.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

#include <linux/videodev2.h>

#define XR_MAX_CAMERAS  16
#define XR_MAX_GROUPS   32
#define XR_MAX_RUNBUFS 128
#define XR_SENSOR_LEN   64

typedef struct {
    int         node;                   /* N from /dev/videoN                */
    unsigned    minor;                  /* char device minor, as tracepoints report it */
    char        path[64];
    unsigned    width;
    unsigned    height;
    unsigned    bytesperline;
    unsigned    nplanes;
    size_t      planesize[VIDEO_MAX_PLANES];
    uint32_t    pixfmt;
    char        sensor[XR_SENSOR_LEN];  /* media entity name of the sensor   */
    const char *role;
    int         nqbuf;                  /* V4L2 indices VIDIOC_QUERYBUF named */
    int         qbuf_xfd[XR_MAX_RUNBUFS]; /* index -> its plane 0 fd in XRService */
} xr_camera_t;

typedef struct {
    int    xfd;                         /* plane 0 descriptor in XRService   */
    int    xfd1;                        /* plane 1 descriptor in XRService   */
    size_t size;
    size_t size1;
} xr_bufref_t;

/* One run of buffers XRService allocated for a camera queue, in allocation order. */
typedef struct {
    size_t       planesize[2];
    int          nbufs;
    xr_bufref_t  buf[XR_MAX_RUNBUFS];
    char         sensor[XR_SENSOR_LEN]; /* from the preceding sensor subdev  */
    xr_camera_t *cam;
    bool         exact;                 /* cam is from VIDIOC_QUERYBUF, not the order */
} xr_group_t;

typedef struct {
    pid_t       pid;
    xr_camera_t cameras[XR_MAX_CAMERAS];
    int         ncameras;
    xr_group_t  groups[XR_MAX_GROUPS];
    int         ngroups;
} xr_state_t;

typedef enum {
    XR_FMT_GREY8,       /* 8-bit mono                                           */
    XR_FMT_NV12,        /* 8-bit Y plane then interleaved UV, same pitch        */
    XR_FMT_YUV420_10P   /* like NV12, but 10-bit MIPI-packed (4 px in 5 bytes)  */
} xr_fmt_t;

/* Where the image really sits in plane 0; V4L2's numbers can be misleading. */
typedef struct {
    xr_fmt_t fmt;
    unsigned pitch;     /* bytes per row                                        */
    unsigned rows;      /* rows in plane 0: luma, plus chroma for YUV           */
    unsigned width;     /* valid pixels per row                                 */
    unsigned height;    /* luma rows                                            */
} xr_layout_t;

/* Scan XRService's descriptors and the media graph. Needs root. */
bool     xr_discover(xr_state_t *st, const char *process, char *err, size_t errn);
unsigned xr_camera_stride(const xr_camera_t *c);
void     xr_camera_layout(const xr_camera_t *c, xr_layout_t *l);
const char *xr_fmt_name(xr_fmt_t f);
void     xr_print(const xr_state_t *st, FILE *f);
void     xr_slugify(const char *in, char *out, size_t n);
