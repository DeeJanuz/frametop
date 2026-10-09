// The OpenVR side of ft-screens (vr.cpp), called from the wlroots compositor (compositor.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ft_dmabuf {
    int width, height;
    uint32_t format;    // DRM_FORMAT_*
    uint64_t modifier;  // DRM_FORMAT_MOD_*
    int n_planes;
    uint32_t offset[4], stride[4];
    int fd[4];
};

// FT_FRONT: a spin (the lazy susan) brought panel `screen` to straight ahead: typing goes there.
enum ft_event_type { FT_MOTION, FT_BUTTON, FT_SCROLL, FT_LEAVE, FT_QUIT, FT_KEY, FT_KEYBOARD_CLOSED, FT_FRONT };

struct ft_event {
    enum ft_event_type type;
    int screen;
    double x, y;      // FT_MOTION: buffer pixels from the top left
    uint32_t button;  // FT_BUTTON: linux BTN_*
    bool pressed;
    bool controller;  // FT_BUTTON: from a hand controller's laser (not the 3D mouse's)
    double dx, dy;    // FT_SCROLL: notches (positive dy: scroll down)
    uint32_t key;     // FT_KEY: linux KEY_* from our keyboard (pressed: down or up)
};

bool ft_vr_init(void);
void ft_vr_shutdown(void);
// Modifiers SteamVR can import for a DRM format. Returns the count (at most max).
int ft_vr_modifiers(uint32_t format, uint64_t *out, int max);
// The screens are showing (by the visibility mode; not counting a wrist-pinned screen).
bool ft_vr_screens_shown(void);
// Frametop is paused for a VR game ("pause on"): everything is hidden, and KWin slows down.
bool ft_vr_paused(void);
// Screen (or floating window) `index` shows now. True without SteamVR (--no-vr).
bool ft_vr_screen_visible(int index);
// How much of a screen you see, for its frame rate (compositor.c): hidden (or out of view),
// in view, or focused (you look at it, or a laser or the mouse is on it). Focused without
// SteamVR (--no-vr) or for an unknown screen.
enum ft_attention { FT_HIDDEN, FT_IN_VIEW, FT_FOCUSED };
enum ft_attention ft_vr_screen_attention(int index);
// The display's refresh rate and the time since its last vsync, in seconds. False without
// them (no SteamVR, or the headset isn't reporting).
bool ft_vr_vsync(double *since, double *hz);
// A panel for screen `index`, width in metres, placed in a row in front of the head.
void ft_vr_screen_create(int index, double metres, int count);
void ft_vr_screen_destroy(int index);
// A remote screen's panel (remote.c): another machine's display, streamed by ft-stream. It
// has a screen's controls and takes the same commands; its frames come through
// ft_vr_screen_present, and its input arrives as events for its index. *restored: it's back
// where it was when its stream last stopped in this run (Remote Displays' Disconnect).
bool ft_vr_remote_create(int index, const char *label, double metres, bool *restored);
// A second ft-screens next to the running desktop (--beside): nothing goes to ft-floatd or
// ft-layout.
void ft_vr_beside(void);
// A spare output's panel, for floating windows (slot numbers from 1): hidden until
// ft-floatd floats a window on it and KWin has the output turned on.
void ft_vr_float_create(int index, int slot);
void ft_vr_float_output(int index, bool on);
// Show a client buffer (identified by `key`) on the screen's panel. False if SteamVR
// can't import it.
bool ft_vr_screen_present(int index, const void *key, const struct ft_dmabuf *buf);
// A buffer is going away: drop its import.
void ft_vr_forget(const void *key);
// Poll panel input and SteamVR events.
void ft_vr_poll(void (*handle)(const struct ft_event *, void *), void *data);
// Our keyboard (keyboard.cpp), for typing on screen `index`: its keys arrive as FT_KEY
// events, and FT_KEYBOARD_CLOSED when its Close key is pressed or the screens hide. False if
// it can't be shown or there's no head pose. With the Steam menu or Steam's own keyboard
// up, it waits and appears when they're gone.
bool ft_vr_keyboard_show(int index);
void ft_vr_keyboard_hide(void);
// A command from the control socket (@ft_screens); writes the reply (see vr.cpp).
void ft_vr_command(const char *command, char *reply, int reply_size);

#ifdef __cplusplus
}
#endif
