#pragma once
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
// Shared-memory Wayland cursor images use ARGB with premultiplied alpha.
// SteamVR's raw overlay texture is RGBA. Respect padding and recover RGB at edges.
static inline bool ft_cursor_rgba(uint8_t *out, const void *data, int w, int h,
                                  size_t stride, uint32_t format) {
    if (!data || w < 1 || h < 1 || w > 512 || h > 512 || stride < (size_t)w*4 ||
        (format != DRM_FORMAT_ARGB8888 && format != DRM_FORMAT_ABGR8888 &&
         format != DRM_FORMAT_XRGB8888 && format != DRM_FORMAT_XBGR8888)) return false;
    const bool swap = format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888;
    const bool opaque = format == DRM_FORMAT_XRGB8888 || format == DRM_FORMAT_XBGR8888;
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        uint32_t v; memcpy(&v, (const uint8_t *)data + (size_t)y*stride + x*4, 4);
        unsigned a = opaque ? 255 : v >> 24;
        unsigned rgb[3] = {swap ? v & 255 : (v >> 16) & 255, (v >> 8) & 255,
                           swap ? (v >> 16) & 255 : v & 255};
        uint8_t *p = out + ((size_t)y*w+x)*4;
        for (int c = 0; c < 3; ++c) { unsigned n = a ? (rgb[c]*255 + a/2)/a : 0; p[c] = n > 255 ? 255 : n; }
        p[3] = a;
    }
    return true;
}
