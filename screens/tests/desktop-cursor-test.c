#include "../desktop-cursor.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    uint32_t input[] = {0x80402010, 0, 0xdeadbeef, 0xff102030, 0x00ffffff, 0};
    uint8_t out[16];
    assert(ft_cursor_rgba(out, input, 2, 2, 12, DRM_FORMAT_ARGB8888));
    assert(out[0] == 128 && out[1] == 64 && out[2] == 32 && out[3] == 128);
    assert(out[4] == 0 && out[7] == 0);
    assert(out[8] == 16 && out[9] == 32 && out[10] == 48 && out[11] == 255);
    assert(out[12] == 0 && out[13] == 0 && out[14] == 0 && out[15] == 0);
    assert(ft_cursor_rgba(out, input, 1, 1, 4, DRM_FORMAT_ABGR8888));
    assert(out[0] == 32 && out[1] == 64 && out[2] == 128);
    assert(ft_cursor_rgba(out, input, 1, 1, 4, DRM_FORMAT_XRGB8888));
    assert(out[0] == 64 && out[1] == 32 && out[2] == 16 && out[3] == 255);
    assert(ft_cursor_rgba(out, input, 1, 1, 4, DRM_FORMAT_XBGR8888));
    assert(out[0] == 16 && out[2] == 64 && out[3] == 255);
    assert(!ft_cursor_rgba(out, input, 1, 1, 3, DRM_FORMAT_ARGB8888));
    assert(!ft_cursor_rgba(out, input, 513, 1, 2052, DRM_FORMAT_ARGB8888));
    assert(!ft_cursor_rgba(out, NULL, 1, 1, 4, DRM_FORMAT_ARGB8888));
    assert(!ft_cursor_rgba(out, input, 1, 1, 4, DRM_FORMAT_RGB565));
    puts("KDE cursor channels, alpha, padding and bounds passed");
}
