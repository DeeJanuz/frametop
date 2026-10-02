// ft-screens: a minimal Wayland compositor that hosts the nested KWin and shows each of
// its screens as its own SteamVR panel. It replaces gamescope for Frametop:
//   - KWin (nested Wayland backend) opens one window per screen. We send each window its
//     size (xdg_toplevel configure), and KWin resizes that screen to match, so every
//     screen has its own resolution and shape (ultrawide, portrait, 4K...). gamescope
//     never sized its windows and drew them all into one canvas of at most 1920x1080.
//   - KWin renders into DMA-BUFs and hands them to us (linux-dmabuf). We draw nothing:
//     each buffer goes to SteamVR as the panel's texture (vr.cpp, ImportDmabuf).
//   - Pointer input from the panels (controller lasers, the 3D mouse) goes to KWin through
//     our seat, as if we were a normal desktop. KWin's nested backend adds our surface
//     coordinates to its output's logical position without undoing its own scale, and its
//     buffers are in pixels, so a panel position in pixels is divided by the screen's KWin
//     scale first ("scale <screen> <s>", from ft-layout).
//
// Usage: ft-screens [--socket NAME] [--control NAME] [--no-vr] [--screen WxH@METRES]...
//                   [--spares N] [-- COMMAND ARGS...]
//   --socket    Wayland socket name in $XDG_RUNTIME_DIR (default ft-screens-0)
//   --control   the control socket's abstract name (default ft_screens)
//   --no-vr     run without SteamVR, for tests next to the running desktop: no panels, no
//               input, and nothing sent to the input relay. Commands still work, and
//               "toplevels" shows what KWin opened.
//   --screen    one per screen, in KWin's order (default: 3440x1440@2.4)
//   --spares    KWin's outputs after the screens: spares for floating windows (ft-floatd
//               turns them on and sizes them; see docs/floating-windows.md)
//   COMMAND     run with WAYLAND_DISPLAY set to our socket (e.g. the Frametop session)
// Runs in the dev container (wlroots 0.20); KWin connects from the host.
#define _GNU_SOURCE
#include "controller-fallback.h"
#include <drm_fourcc.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#define WLR_USE_UNSTABLE
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/render/dmabuf.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#include "vr.h"
#include "desktop-mouse.h"
#include "desktop-cursor.h"
#include "cursor-cache.h"

#define MAX_SCREENS FT_MOUSE_OUTPUTS  // screens and spare outputs

struct config {
    int width, height;
    double metres;
};

struct server;

// One KWin window = one screen.
struct screen {
    struct server *server;
    int index;
    struct wlr_xdg_toplevel *toplevel;
    struct wlr_buffer *held;      // on the panel now; unlocked when the next one arrives
    bool frame_pending;           // a commit waits for its frame callback
    unsigned commits;             // buffers committed, and the last one's size ("toplevels")
    int buffer_width, buffer_height;
    struct wlr_xdg_toplevel_decoration_v1 *decoration;  // answered on the first commit
    struct wl_listener commit, destroy, decoration_destroy, set_title;
};

// Per client buffer: forget its import when it goes away.
struct tracked_buffer {
    struct wlr_buffer *buffer;
    struct wl_listener destroy;
    struct wl_list link;
};

struct server {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wlr_seat *seat;
    struct wlr_keyboard keyboard;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_xdg_decoration_manager_v1 *decoration;
    struct wl_listener new_toplevel, new_decoration;
    struct screen *screens[MAX_SCREENS];
    struct config config[MAX_SCREENS];
    double scale[MAX_SCREENS];  // KWin's scale for each screen (panel pixels per logical unit)
    int n_config, n_screens;  // n_config: the screens; the outputs after them are spares
    int spares;
    struct wl_list buffers;  // tracked_buffer
    struct wl_event_source *tick;
    struct screen *pointer_focus;
    bool desktop_mouse, cursor_ready, cursor_frame_pending;
    struct ft_controller_fallback controller_fallback;
    bool controller_desktop;
    uint32_t controller_buttons;
    struct ft_cursor_cache cursor_cache;
    const char *cursor_error;
    const char *cursor_buffer;
    struct wlr_buffer *cursor_held;
    struct ft_mouse mouse;
    struct wlr_surface *cursor_surface;
    int cursor_hx, cursor_hy;
    struct wl_listener request_cursor, cursor_commit, cursor_destroy;
    unsigned long mouse_motions, mouse_clicks, mouse_wheels, vr_mouse_ignored;
    pid_t child;
    // Where typing goes: the screens after a click on one, Steam after a click on another
    // panel. The input relay grabs the keyboards while it's the screens (see keys_update).
    bool keys_clicked;    // the last click was on a screen
    bool keys_desktop;    // ...and the screens are showing: typing goes to the desktop
    int relay_fd;         // unbound, so the relay can't reply into our control socket
    uint32_t relay_sent;  // when the relay last heard from us (ms)
    unsigned ticks;
    // Our keyboard ("vrkeyboard" commands from the input relay; see keyboard_command).
    int kb_screen;         // the screen it's open for, -1 closed
    bool kb_auto;          // opened for a focused text field (not by a button)
    unsigned kb_close_at;  // ticks: close it then (a text field lost focus), 0 not
    bool vr;               // connected to SteamVR (not --no-vr)
};

static uint32_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// ---------------------------------------------------------------- buffers

static void buffer_destroyed(struct wl_listener *l, void *data) {
    struct tracked_buffer *t = wl_container_of(l, t, destroy);
    ft_vr_forget(t->buffer);
    wl_list_remove(&t->destroy.link);
    wl_list_remove(&t->link);
    free(t);
}

static void track_buffer(struct server *s, struct wlr_buffer *buffer) {
    struct tracked_buffer *t;
    wl_list_for_each(t, &s->buffers, link) if (t->buffer == buffer) return;
    t = calloc(1, sizeof *t);
    t->buffer = buffer;
    t->destroy.notify = buffer_destroyed;
    wl_signal_add(&buffer->events.destroy, &t->destroy);
    wl_list_insert(&s->buffers, &t->link);
}

// ---------------------------------------------------------------- screens

static void screen_commit(struct wl_listener *l, void *data) {
    struct screen *sc = wl_container_of(l, sc, commit);
    struct wlr_xdg_surface *xdg = sc->toplevel->base;
    if (xdg->initial_commit) {
        // First commit: tell KWin the size of this screen.
        const struct config spare = {640, 480, 0.5};  // until ft-floatd sizes it
        const struct config *c = sc->index < sc->server->n_config ? &sc->server->config[sc->index] : &spare;
        wlr_xdg_toplevel_set_size(sc->toplevel, c->width, c->height);
        wlr_xdg_toplevel_set_activated(sc->toplevel, true);
        if (sc->decoration)
            wlr_xdg_toplevel_decoration_v1_set_mode(sc->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        return;
    }
    struct wlr_buffer *buffer = xdg->surface->current.buffer;
    static int logged;
    if (logged < 6) {
        ++logged;
        wlr_log(WLR_INFO, "screen %d: commit, buffer %p (%dx%d), mapped %d", sc->index + 1, (void *)buffer,
                buffer ? buffer->width : 0, buffer ? buffer->height : 0, xdg->surface->mapped);
    }
    if (!buffer) return;
    sc->frame_pending = true;
    ++sc->commits;
    sc->buffer_width = buffer->width, sc->buffer_height = buffer->height;
    if (buffer == sc->held) return;
    struct wlr_dmabuf_attributes a;
    if (!wlr_buffer_get_dmabuf(buffer, &a)) {
        static bool warned;
        if (!warned) wlr_log(WLR_ERROR, "screen %d: not a DMA-BUF (shm?); skipped", sc->index + 1);
        warned = true;
        return;
    }
    struct ft_dmabuf b = {.width = a.width, .height = a.height, .format = a.format, .modifier = a.modifier,
                          .n_planes = a.n_planes};
    for (int i = 0; i < a.n_planes && i < 4; ++i) {
        b.offset[i] = a.offset[i];
        b.stride[i] = a.stride[i];
        b.fd[i] = a.fd[i];
    }
    track_buffer(sc->server, buffer);
    if (!ft_vr_screen_present(sc->index, buffer, &b)) return;
    // Keep this buffer until the next frame replaces it, so SteamVR never samples a
    // buffer KWin is drawing into; then let KWin have the previous one back.
    wlr_buffer_lock(buffer);
    if (sc->held) wlr_buffer_unlock(sc->held);
    sc->held = buffer;
}

static void screen_destroy(struct wl_listener *l, void *data) {
    struct screen *sc = wl_container_of(l, sc, destroy);
    wlr_log(WLR_INFO, "screen %d closed", sc->index + 1);
    if (sc->held) wlr_buffer_unlock(sc->held);
    ft_vr_screen_destroy(sc->index);
    if (sc->server->pointer_focus == sc) sc->server->pointer_focus = NULL;
    sc->server->mouse.outputs[sc->index].enabled = false;
    if (sc->server->mouse.grab == sc->index) sc->server->mouse.buttons = 0, sc->server->mouse.grab = -1;
    if (sc->decoration) wl_list_remove(&sc->decoration_destroy.link);
    sc->server->screens[sc->index] = NULL;
    wl_list_remove(&sc->commit.link);
    wl_list_remove(&sc->destroy.link);
    wl_list_remove(&sc->set_title.link);
    free(sc);
}

// KWin titles each window "KDE Wayland Compositor <output name>", with "- Output disabled"
// after it while that output is off.
static void screen_title(struct wl_listener *l, void *data) {
    struct screen *sc = wl_container_of(l, sc, set_title);
    const char *title = sc->toplevel->title ? sc->toplevel->title : "";
    wlr_log(WLR_INFO, "screen %d: \"%s\"", sc->index + 1, title);
    if (sc->index >= sc->server->n_config) ft_vr_float_output(sc->index, !strstr(title, "Output disabled"));
}

static void new_toplevel(struct wl_listener *l, void *data) {
    struct server *s = wl_container_of(l, s, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    int index = 0;
    while (index < MAX_SCREENS && s->screens[index]) ++index;
    if (index == MAX_SCREENS) {
        wlr_log(WLR_ERROR, "more than %d screens; ignoring one", MAX_SCREENS);
        return;
    }
    struct screen *sc = calloc(1, sizeof *sc);
    sc->server = s;
    sc->index = index;
    sc->toplevel = toplevel;
    s->screens[index] = sc;
    if (index >= s->n_config) {
        wlr_log(WLR_INFO, "screen %d: KWin window, a spare output (floating window %d)", index + 1,
                index - s->n_config + 1);
        ft_vr_float_create(index, index - s->n_config + 1);
    } else {
        const struct config *c = &s->config[index];
        wlr_log(WLR_INFO, "screen %d: KWin window, %dx%d, %.2f m wide", index + 1, c->width, c->height, c->metres);
        ft_vr_screen_create(index, c->metres, s->n_config);
    }
    sc->commit.notify = screen_commit;
    wl_signal_add(&toplevel->base->surface->events.commit, &sc->commit);
    sc->destroy.notify = screen_destroy;
    wl_signal_add(&toplevel->events.destroy, &sc->destroy);
    sc->set_title.notify = screen_title;
    wl_signal_add(&toplevel->events.set_title, &sc->set_title);
}

// KWin asks for server-side decorations for its screens; we draw none. It asks before its
// first commit, when a configure isn't allowed yet, so the answer waits for that commit.
static void decoration_destroyed(struct wl_listener *l, void *data) {
    struct screen *sc = wl_container_of(l, sc, decoration_destroy);
    wl_list_remove(&sc->decoration_destroy.link);
    sc->decoration = NULL;
}

static void new_decoration(struct wl_listener *l, void *data) {
    struct server *s = wl_container_of(l, s, new_decoration);
    struct wlr_xdg_toplevel_decoration_v1 *d = data;
    for (int i = 0; i < MAX_SCREENS; ++i) {
        struct screen *sc = s->screens[i];
        if (!sc || sc->toplevel != d->toplevel) continue;
        sc->decoration = d;
        sc->decoration_destroy.notify = decoration_destroyed;
        wl_signal_add(&d->events.destroy, &sc->decoration_destroy);
        if (d->toplevel->base->initialized)
            wlr_xdg_toplevel_decoration_v1_set_mode(d, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
}

// ---------------------------------------------------------------- input from the panels

static void panel_key(struct server *s, uint32_t code, bool pressed);

static void deliver_pointer_event(const struct ft_event *e, void *data) {
    struct server *s = data;
    if (e->type == FT_QUIT) {
        wl_display_terminate(s->display);
        return;
    }
    if (e->type == FT_KEY) {
        panel_key(s, e->key, e->pressed);
        return;
    }
    if (e->type == FT_KEYBOARD_CLOSED) {
        if (s->kb_screen >= 0) wlr_log(WLR_INFO, "keyboard closed");
        s->kb_screen = -1;
        s->kb_close_at = 0;
        return;
    }
    if (e->screen < 0 || e->screen >= MAX_SCREENS || !s->screens[e->screen]) return;
    struct screen *sc = s->screens[e->screen];
    struct wlr_surface *surface = sc->toplevel->base->surface;
    const uint32_t t = now_ms();
    const double x = e->x / s->scale[e->screen], y = e->y / s->scale[e->screen];  // KWin's units
    switch (e->type) {
        case FT_MOTION:
        case FT_BUTTON:
            if (s->pointer_focus != sc) {
                // KWin's nested backend ignores the position in wl_pointer.enter, and wlroots
                // drops a motion to the position it entered at, so KWin would keep its old
                // pointer until the next move: a press right after crossing onto another
                // screen landed where the pointer had been. Entering one unit off makes the
                // motion below go through.
                wlr_seat_pointer_notify_enter(s->seat, surface, x + 1, y);
                s->pointer_focus = sc;
            }
            wlr_seat_pointer_notify_motion(s->seat, t, x, y);
            if (e->type == FT_BUTTON) {
                wlr_seat_pointer_notify_button(s->seat, t, e->button,
                                               e->pressed ? WL_POINTER_BUTTON_STATE_PRESSED
                                                          : WL_POINTER_BUTTON_STATE_RELEASED);
                if (e->pressed) {
                    wlr_seat_keyboard_notify_enter(s->seat, surface, NULL, 0, NULL);
                    s->keys_clicked = true;
                }
            }
            break;
        case FT_SCROLL:
            if (s->pointer_focus != sc) break;
            if (sc->index >= s->n_config && e->dy != 0 &&
                (wlr_keyboard_get_modifiers(&s->keyboard) & WLR_MODIFIER_LOGO)) {
                // Meta+scroll on a floating window: its scale, bigger or smaller (ft-floatd).
                char msg[48];
                snprintf(msg, sizeof msg, "scale %d %d", sc->index + 1, e->dy < 0 ? 1 : -1);
                struct sockaddr_un addr = {.sun_family = AF_UNIX};
                const char name[] = "frametop_float";
                memcpy(addr.sun_path + 1, name, sizeof name - 1);
                sendto(s->relay_fd, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&addr,
                       offsetof(struct sockaddr_un, sun_path) + 1 + sizeof name - 1);
                break;
            }
            if (e->dy != 0)
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_VERTICAL_SCROLL, e->dy * 15,
                                             (int32_t)llround(e->dy * 120), WL_POINTER_AXIS_SOURCE_WHEEL,
                                             WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            if (e->dx != 0)
                wlr_seat_pointer_notify_axis(s->seat, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL, e->dx * 15,
                                             (int32_t)llround(e->dx * 120), WL_POINTER_AXIS_SOURCE_WHEEL,
                                             WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
            break;
        case FT_LEAVE:
            if (s->pointer_focus == sc) {
                wlr_seat_pointer_notify_clear_focus(s->seat);
                s->pointer_focus = NULL;
            }
            break;
        default:
            break;
    }
    wlr_seat_pointer_notify_frame(s->seat);
}

static void update_controller_fallback(struct server *s) {
    bool active = s->desktop_mouse && !s->mouse.buttons &&
        ft_fallback_active(&s->controller_fallback, now_ms());
    if (active == s->controller_desktop) return;
    // Release controller-owned presses before the native seat takes over.
    for (unsigned bit = 0; bit < 8; ++bit)
        if (s->controller_buttons & (1u << bit))
            wlr_seat_pointer_notify_button(s->seat, now_ms(), BTN_LEFT+bit,
                                           WL_POINTER_BUTTON_STATE_RELEASED);
    s->controller_buttons = 0;
    wlr_seat_pointer_notify_clear_focus(s->seat);
    wlr_seat_pointer_notify_frame(s->seat);
    s->pointer_focus = NULL;
    s->controller_desktop = active;
    wlr_log(WLR_INFO, "desktop controller fallback %s (mouse %s, telemetry %s)",
            active ? "active" : "inactive", s->controller_fallback.present ? "present" : "absent",
            s->controller_fallback.known && (uint32_t)(now_ms()-s->controller_fallback.received)<3000 ? "fresh" : "unknown/stale");
    ft_vr_cursor_move(-1, 0, 0, 1, false);
}

static void handle_vr_event(const struct ft_event *e, void *data) {
    struct server *s = data;
    // Native mouse mode owns the desktop seat. Controller lasers still operate
    // SteamVR, games and FrameTop's separate screen placement controls.
    update_controller_fallback(s);
    if (s->desktop_mouse && !s->controller_desktop && e->screen < s->n_config &&
        (e->type == FT_MOTION || e->type == FT_BUTTON || e->type == FT_SCROLL || e->type == FT_LEAVE)) { ++s->vr_mouse_ignored; return; }
    if (s->controller_desktop && e->screen < s->n_config && e->type == FT_BUTTON && e->button >= BTN_LEFT && e->button < BTN_LEFT+8) {
        uint32_t bit = 1u << (e->button-BTN_LEFT);
        if (e->pressed) s->controller_buttons |= bit;
        else s->controller_buttons &= ~bit;
    }
    deliver_pointer_event(e, data);
}

static void desktop_motion(struct server *s) {
    // A real mouse packet wins immediately, before the next hotplug heartbeat.
    if (s->controller_desktop) {
        ft_fallback_presence(&s->controller_fallback, true, s->controller_fallback.enabled, now_ms());
        update_controller_fallback(s);
    }
    int index = ft_mouse_target(&s->mouse);
    if (index < 0 || !s->screens[index]) return;
    const struct ft_mouse_output *o = &s->mouse.outputs[index];
    struct ft_event event = {.type = FT_MOTION, .screen = index,
        .x = (s->mouse.x-o->x)*o->scale, .y = (s->mouse.y-o->y)*o->scale};
    deliver_pointer_event(&event, s);
    const struct ft_mouse_output *display = &s->mouse.outputs[s->mouse.screen];
    ft_vr_cursor_move(s->mouse.screen, (s->mouse.x-display->x)*display->scale,
                     (s->mouse.y-display->y)*display->scale, display->scale, s->desktop_mouse);
}

static void cursor_hide(struct server *s) {
    ft_vr_cursor_image(NULL, 0, 0, 0, 0, 1);
    if (s->cursor_held) wlr_buffer_unlock(s->cursor_held);
    s->cursor_held = NULL;
}
static void cursor_update(struct server *s) {
    struct wlr_surface *surface = s->cursor_surface;
    struct wlr_buffer *buffer = surface ? ft_cursor_cached(&s->cursor_cache, surface) : NULL;
    s->cursor_ready = false;
    s->cursor_error = "no cached KDE cursor buffer";
    if (surface) s->cursor_frame_pending = true;
    s->cursor_buffer = "none";
    if (!buffer) { cursor_hide(s); return; }
    if (buffer->width < 1 || buffer->height < 1 || buffer->width > 512 || buffer->height > 512) {
        cursor_hide(s); return;
    }
    int scale = surface->current.scale > 0 ? surface->current.scale : 1;
    if (scale > 8 || s->cursor_hx < -512 || s->cursor_hy < -512 || s->cursor_hx > 512 || s->cursor_hy > 512) {
        s->cursor_error = "invalid cursor scale or hotspot";
        cursor_hide(s); return;
    }
    struct wlr_dmabuf_attributes dma;
    if (wlr_buffer_get_dmabuf(buffer, &dma)) {
        s->cursor_buffer = "dmabuf";
        struct ft_dmabuf b = {.width = dma.width, .height = dma.height, .format = dma.format,
                             .modifier = dma.modifier, .n_planes = dma.n_planes};
        for (int i = 0; i < dma.n_planes && i < 4; ++i) {
            b.offset[i] = dma.offset[i]; b.stride[i] = dma.stride[i]; b.fd[i] = dma.fd[i];
        }
        track_buffer(s, buffer);
        s->cursor_ready = ft_vr_cursor_dmabuf(buffer, &b, s->cursor_hx*scale, s->cursor_hy*scale, scale);
        s->cursor_error = s->cursor_ready ? "" : "SteamVR rejected GPU cursor";
        if (s->cursor_ready) {
            wlr_buffer_lock(buffer);
            if (s->cursor_held) wlr_buffer_unlock(s->cursor_held);
            s->cursor_held = buffer;
        } else cursor_hide(s);
        return;
    }
    s->cursor_buffer = "shm";
    void *data; uint32_t format; size_t stride;
    if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
        s->cursor_error = "KDE cursor buffer cannot be read";
        cursor_hide(s); return;
    }
    uint8_t *rgba = malloc((size_t)buffer->width*buffer->height*4);
    s->cursor_error = "unsupported cursor format or allocation failed";
    if (rgba && ft_cursor_rgba(rgba, data, buffer->width, buffer->height, stride, format)) {
        s->cursor_error = "SteamVR rejected cursor texture";
        s->cursor_ready = ft_vr_cursor_image(rgba, buffer->width, buffer->height,
                                            s->cursor_hx*scale, s->cursor_hy*scale, scale);
        if (s->cursor_ready) s->cursor_error = "";
    }
    free(rgba);
    wlr_buffer_end_data_ptr_access(buffer);
    if (!s->cursor_ready) cursor_hide(s);
    else if (s->cursor_held) { wlr_buffer_unlock(s->cursor_held); s->cursor_held = NULL; }
}

static void cursor_committed(struct wl_listener *l, void *data) {
    struct server *s = wl_container_of(l, s, cursor_commit);
    cursor_update(s);
}
static void cursor_destroyed(struct wl_listener *l, void *data) {
    struct server *s = wl_container_of(l, s, cursor_destroy);
    wl_list_remove(&s->cursor_commit.link); wl_list_remove(&s->cursor_destroy.link);
    s->cursor_surface = NULL; s->cursor_ready = false;
    cursor_hide(s);
}
static void cursor_requested(struct wl_listener *l, void *data) {
    struct server *s = wl_container_of(l, s, request_cursor);
    struct wlr_seat_pointer_request_set_cursor_event *e = data;
    if (e->seat_client != s->seat->pointer_state.focused_client) return;
    if (s->cursor_surface) {
        wl_list_remove(&s->cursor_commit.link); wl_list_remove(&s->cursor_destroy.link);
    }
    s->cursor_surface = e->surface; s->cursor_hx = e->hotspot_x; s->cursor_hy = e->hotspot_y;
    if (e->surface) {
        s->cursor_commit.notify = cursor_committed;
        s->cursor_destroy.notify = cursor_destroyed;
        wl_signal_add(&e->surface->events.commit, &s->cursor_commit);
        wl_signal_add(&e->surface->events.destroy, &s->cursor_destroy);
    }
    cursor_update(s);
}

// Tell the input relay where typing goes, on a change and every second: while it's the
// desktop, the relay grabs pass-through keyboards so SteamVR (and the Steam app with
// gamescope's focus) doesn't get the keys too. Without word from us for a few seconds,
// the relay gives the keyboards back, so a closed desktop doesn't keep them.
static void keys_update(struct server *s) {
    if (!s->vr) return;  // the isolated test desktop must never route live keyboards
    if (!s->vr) return;  // a test instance leaves the running desktop's keyboards alone
    const bool desktop = s->keys_clicked && ft_vr_screens_shown();
    const uint32_t t = now_ms();
    if (desktop == s->keys_desktop && t - s->relay_sent < 1000) return;
    if (desktop != s->keys_desktop) wlr_log(WLR_INFO, "typing goes to %s", desktop ? "the desktop" : "Steam");
    s->keys_desktop = desktop;
    s->relay_sent = t;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    const char name[] = "frametop_relay";
    memcpy(addr.sun_path + 1, name, sizeof name - 1);
    const char *msg = desktop ? "keyboard desktop" : "keyboard steam";
    sendto(s->relay_fd, msg, strlen(msg), MSG_DONTWAIT, (struct sockaddr *)&addr,
           offsetof(struct sockaddr_un, sun_path) + 1 + sizeof name - 1);
}

// Every ~11 ms (90 Hz): SteamVR events, and frame callbacks for screens that committed.
static int tick(void *data) {
    struct server *s = data;
    update_controller_fallback(s);
    ft_vr_poll(handle_vr_event, s);
    if (++s->ticks % 9 == 0) keys_update(s);
    if (s->kb_close_at && s->ticks >= s->kb_close_at) {
        s->kb_close_at = 0;
        if (s->kb_screen >= 0) {
            ft_vr_keyboard_hide();
            s->kb_screen = -1;
        }
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (s->cursor_surface && s->cursor_frame_pending) {
        s->cursor_frame_pending = false;
        wlr_surface_send_frame_done(s->cursor_surface, &now);
    }
    for (int i = 0; i < MAX_SCREENS; ++i) {
        struct screen *sc = s->screens[i];
        if (sc && sc->frame_pending) {
            sc->frame_pending = false;
            wlr_surface_send_frame_done(sc->toplevel->base->surface, &now);
        }
    }
    wl_event_source_timer_update(s->tick, 11);
    return 0;
}

static int child_exited(int sig, void *data) {
    struct server *s = data;
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
        if (pid == s->child) {
            wlr_log(WLR_INFO, "session exited");
            wl_display_terminate(s->display);
        }
    return 0;
}

static bool key_held(const struct wlr_keyboard *kb, uint32_t code) {
    for (size_t i = 0; i < kb->num_keycodes; i++)
        if (kb->keycodes[i] == code) return true;
    return false;
}

// One key to the focused screen, through the seat's keyboard so its xkb state and
// modifiers stay right.
static void send_key(struct server *s, uint32_t code, int pressed) {
    struct wlr_keyboard_key_event ev = {
        .time_msec = now_ms(), .keycode = code, .update_state = true,
        .state = pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED};
    wlr_keyboard_notify_key(&s->keyboard, &ev);
    wlr_seat_keyboard_notify_modifiers(s->seat, &s->keyboard.modifiers);
    wlr_seat_keyboard_notify_key(s->seat, ev.time_msec, code, ev.state);
}

// Keys from the input relay (physical keyboards): "key <evdev code> <1 press|0 release>".
// They go to the screen KWin has keyboard focus on (the last one clicked), while typing
// goes to the desktop (keys_update). The release of a key the desktop got the press for
// always goes through, or the key stays held there (a modifier held as typing moves to
// Steam would otherwise modify every key typed after it).
static void handle_key(struct server *s, uint32_t code, int value, char *reply, int size) {
    if (value == 2) return (void)snprintf(reply, size, "ok repeat ignored");  // KWin repeats itself
    if (value || !key_held(&s->keyboard, code)) {
        if (!s->seat->keyboard_state.focused_surface) return (void)snprintf(reply, size, "ok no focus");
        if (!s->keys_desktop) return (void)snprintf(reply, size, "ok typing goes to Steam");
    }
    send_key(s, code, value);
    snprintf(reply, size, "ok");
}

// Our keyboard (keyboard.cpp). The input relay sends "vrkeyboard show" when a text field
// on the desktop gets focus (and its setting allows), "vrkeyboard hide" when it loses it,
// and "vrkeyboard toggle" for a mapped button; ft-layout sends "vrkeyboard close" when it
// resets the layout. It opens for the screen KWin has keyboard focus on. A hide closes only a keyboard a text field opened, and waits a moment, so
// moving between text fields doesn't close and reopen it.
static int focused_screen(struct server *s) {
    struct wlr_surface *focus = s->seat->keyboard_state.focused_surface;
    for (int i = 0; i < MAX_SCREENS; ++i)
        if (s->screens[i] && s->screens[i]->toplevel->base->surface == focus) return i;
    if (s->pointer_focus) return s->pointer_focus->index;
    for (int i = 0; i < MAX_SCREENS; ++i)
        if (s->screens[i]) return i;
    return -1;
}

static void keyboard_command(struct server *s, const char *what, char *reply, int size) {
    const bool open = s->kb_screen >= 0;
    if (strcmp(what, "hide") == 0) {
        if (open && s->kb_auto && !s->kb_close_at) s->kb_close_at = s->ticks + 30;  // ~1/3 s
        return (void)snprintf(reply, size, "ok");
    }
    if (strcmp(what, "close") == 0) {  // however it opened, now (ft-layout apply: a reset)
        if (open) wlr_log(WLR_INFO, "keyboard closed (reset)");
        ft_vr_keyboard_hide();
        s->kb_screen = -1;
        s->kb_close_at = 0;
        return (void)snprintf(reply, size, "ok");
    }
    const bool toggle = strcmp(what, "toggle") == 0;
    if (!toggle && strcmp(what, "show") != 0) return (void)snprintf(reply, size, "error show|hide|toggle|close");
    s->kb_close_at = 0;
    if (open && toggle) {
        ft_vr_keyboard_hide();
        s->kb_screen = -1;
        return (void)snprintf(reply, size, "ok closed");
    }
    if (open) return (void)snprintf(reply, size, "ok open");
    if (!ft_vr_screens_shown()) return (void)snprintf(reply, size, "ok screens hidden");
    const int screen = focused_screen(s);
    if (screen < 0 || !ft_vr_keyboard_show(screen)) return (void)snprintf(reply, size, "error not shown");
    wlr_log(WLR_INFO, "keyboard open for screen %d (%s)", screen + 1, toggle ? "button" : "text field");
    s->kb_screen = screen;
    s->kb_auto = !toggle;
    snprintf(reply, size, "ok opened");
}

// A key from our keyboard, for the focused screen. Its release always goes through, so
// no key stays held.
static void panel_key(struct server *s, uint32_t code, bool pressed) {
    if (pressed ? s->seat->keyboard_state.focused_surface != NULL : key_held(&s->keyboard, code))
        send_key(s, code, pressed);
}
// Control socket: abstract datagram @ft_screens. Here: "size <screen> <w> <h>" (a new
// resolution, live), "scale <screen> <s>" (KWin's scale for it, from ft-layout),
// "toplevels" (-> "ok <count>" and a line per KWin window: "<screen> <w>x<h> <commits>
// <title>"), "input <screen> move|down|up|leave [x y [left|right|middle]]" (pointer input
// as if from that panel, x and y in its pixels; for tests, mostly with --no-vr),
// "key <code> <value>", "vrkeyboard show|hide|toggle|close" (our keyboard, from the input
// relay), and "click <overlay key>" (from the pointer helper: a mouse click landed on
// that panel, "-" for none); the rest is in vr.cpp (ft_vr_command).
static int control_readable(int fd, uint32_t mask, void *data) {
    struct server *s = data;
    char buf[512], reply[2048];
    struct sockaddr_un from;
    socklen_t len = sizeof from;
    ssize_t n;
    while ((n = recvfrom(fd, buf, sizeof buf - 1, MSG_DONTWAIT, (struct sockaddr *)&from, &len)) > 0) {
        buf[n] = 0;
        unsigned code;
        int value, index, w, h;
        double scale, wheel_x, wheel_y;
        double mx, my, mw, mh;
        char mode[16], tail;
        if (strcmp(buf, "mouse-presence?") == 0) {
            snprintf(reply, sizeof reply,
                     "{\"supported\":true,\"known\":%s,\"present\":%s,\"enabled\":%s,\"controllerDesktop\":%s,\"fresh\":%s}",
                     s->controller_fallback.known ? "true" : "false",
                     s->controller_fallback.present ? "true" : "false",
                     s->controller_fallback.enabled ? "true" : "false",
                     s->controller_desktop ? "true" : "false",
                     s->controller_fallback.known && (uint32_t)(now_ms()-s->controller_fallback.received)<3000 ? "true" : "false");
        } else if (sscanf(buf, "mouse-presence %d %d %c", &index, &value, &tail) == 2) {
            if ((index != 0 && index != 1) || (value != 0 && value != 1))
                snprintf(reply, sizeof reply, "error invalid presence");
            else {
                ft_fallback_presence(&s->controller_fallback, index, value, now_ms());
                update_controller_fallback(s);
                snprintf(reply, sizeof reply, "ok");
            }
        } else if (strcmp(buf, "mouse?") == 0) {
            snprintf(reply, sizeof reply,
                "{\"supported\":true,\"mode\":\"%s\",\"screen\":%d,\"x\":%.3f,\"y\":%.3f,"
                "\"buttons\":%u,\"cursorReady\":%s,\"cursorSource\":\"kde\",\"cursorCache\":true,\"cursorBuffer\":\"%s\",\"cursorError\":\"%s\","
                "\"motionEvents\":%lu,\"buttonEvents\":%lu,\"wheelEvents\":%lu,\"controllerEventsIgnored\":%lu}",
                s->desktop_mouse ? "desktop" : "spatial", s->mouse.screen+1, s->mouse.x, s->mouse.y,
                s->mouse.buttons, s->cursor_ready ? "true" : "false", s->cursor_buffer ? s->cursor_buffer : "none", s->cursor_error ? s->cursor_error : "no cursor request", s->mouse_motions,
                s->mouse_clicks, s->mouse_wheels, s->vr_mouse_ignored);
        } else if (sscanf(buf, "mouse-output-off %d %c", &index, &tail) == 1) {
            if (index < 1 || index > MAX_SCREENS || s->mouse.buttons)
                snprintf(reply, sizeof reply, "error invalid mouse output or held button");
            else { s->mouse.outputs[index-1].enabled = false; snprintf(reply, sizeof reply, "ok"); }
        } else if (sscanf(buf, "mouse-layout %d %lf %lf %lf %lf %lf %c", &index, &mx, &my, &mw, &mh, &scale, &tail) == 6) {
            if (index < 1 || index > MAX_SCREENS || !s->screens[index-1] ||
                !ft_mouse_output_set(&s->mouse, index-1, mx, my, mw, mh, scale))
                snprintf(reply, sizeof reply, "error invalid mouse output or held button");
            else { s->scale[index-1] = scale; snprintf(reply, sizeof reply, "ok"); }
        } else if (sscanf(buf, "mouse-mode %15s %c", mode, &tail) == 1) {
            if (s->mouse.buttons || (strcmp(mode, "desktop") && strcmp(mode, "spatial")))
                snprintf(reply, sizeof reply, "error mouse mode or held button");
            else if (!strcmp(mode, "desktop") && !ft_mouse_move(&s->mouse, 0, 0))
                snprintf(reply, sizeof reply, "error configure mouse layout first");
            else {
                s->desktop_mouse = !strcmp(mode, "desktop");
                if (s->desktop_mouse) desktop_motion(s);
                else {
                    wlr_seat_pointer_notify_clear_focus(s->seat); s->pointer_focus = NULL;
                    wlr_seat_pointer_notify_frame(s->seat);
                    ft_vr_cursor_move(-1, 0, 0, 1, false);
                }
                snprintf(reply, sizeof reply, "ok");
            }
        } else if (sscanf(buf, "mouse-move %lf %lf %c", &mx, &my, &tail) == 2) {
            if (!s->desktop_mouse || (s->vr && !ft_vr_screens_shown()) || !ft_mouse_move(&s->mouse, mx, my))
                snprintf(reply, sizeof reply, "error desktop mouse unavailable or invalid motion");
            else { desktop_motion(s); ++s->mouse_motions; snprintf(reply, sizeof reply, "ok"); }
        } else if (sscanf(buf, "mouse-position %d %lf %lf %c", &index, &mx, &my, &tail) == 3) {
            if (!s->desktop_mouse || s->mouse.buttons || index < 1 || index > MAX_SCREENS ||
                !s->mouse.outputs[index-1].enabled || !isfinite(mx) || !isfinite(my) ||
                mx < 0 || my < 0 || mx >= s->mouse.outputs[index-1].width || my >= s->mouse.outputs[index-1].height)
                snprintf(reply, sizeof reply, "error mouse position or held button");
            else {
                ft_mouse_position(&s->mouse, s->mouse.outputs[index-1].x+mx, s->mouse.outputs[index-1].y+my);
                desktop_motion(s); snprintf(reply, sizeof reply, "ok");
            }
        } else if (sscanf(buf, "mouse-button %u %d %c", &code, &value, &tail) == 2) {
            if (!s->desktop_mouse || (value != 0 && value != 1) ||
                (s->vr && !ft_vr_screens_shown() && value) || code < BTN_LEFT || code > BTN_TASK)
                snprintf(reply, sizeof reply, "error mouse button or hidden desktop");
            else {
                desktop_motion(s);
                int target = ft_mouse_target(&s->mouse);
                const struct ft_mouse_output *o = target >= 0 ? &s->mouse.outputs[target] : NULL;
                if (!o || !s->screens[target]) snprintf(reply, sizeof reply, "error no mouse surface");
                else {
                    struct ft_event event = {.type = FT_BUTTON, .screen = target, .button = code, .pressed = value,
                        .x = (s->mouse.x-o->x)*o->scale, .y = (s->mouse.y-o->y)*o->scale};
                    deliver_pointer_event(&event, s);
                    ft_mouse_button(&s->mouse, code, value); ++s->mouse_clicks;
                    if (!s->mouse.buttons) desktop_motion(s);
                    snprintf(reply, sizeof reply, "ok");
                }
            }
        } else if (sscanf(buf, "mouse-wheel %lf %lf %c", &wheel_x, &wheel_y, &tail) == 2) {
            int target = ft_mouse_target(&s->mouse);
            if (!s->desktop_mouse || (s->vr && !ft_vr_screens_shown()) || target < 0 || !s->screens[target] ||
                !s->mouse.outputs[target].enabled ||
                !isfinite(wheel_x) || !isfinite(wheel_y) || fabs(wheel_x)>120 || fabs(wheel_y)>120)
                snprintf(reply, sizeof reply, "error mouse wheel or hidden desktop");
            else {
                desktop_motion(s);
                struct ft_event event = {.type = FT_SCROLL, .screen = target, .dx = wheel_x, .dy = -wheel_y};
                deliver_pointer_event(&event, s); ++s->mouse_wheels; snprintf(reply, sizeof reply, "ok");
            }
        } else if (sscanf(buf, "size %d %d %d", &index, &w, &h) == 3) {
            // A new resolution for a screen, live: KWin resizes the screen to match. (KWin makes
            // it this size times its scale; ft-floatd sends spares' sizes divided by theirs.)
            const int min_w = index - 1 < s->n_config ? 320 : 64, min_h = index - 1 < s->n_config ? 200 : 64;
            if (index < 1 || index > MAX_SCREENS || !s->screens[index - 1] || w < min_w || h < min_h || w > 16384 ||
                h > 16384) {
                snprintf(reply, sizeof reply, "error bad screen or size");
            } else {
                // A screen's size is even: KWin's nested backend gives a scaled screen a whole
                // buffer scale (1.5 -> 2), and a buffer that isn't a multiple of it is a protocol
                // error that disconnects KWin. (ft-floatd rounds spares' sizes for their scale.)
                if (index - 1 < s->n_config) w += w & 1, h += h & 1;
                if (index - 1 < s->n_config) s->config[index - 1].width = w, s->config[index - 1].height = h;
                wlr_xdg_toplevel_set_size(s->screens[index - 1]->toplevel, w, h);
                snprintf(reply, sizeof reply, "ok");
            }
        } else if (sscanf(buf, "scale %d %lf", &index, &scale) == 2) {
            if (index < 1 || index > MAX_SCREENS || !(scale >= 0.25 && scale <= 8)) {
                snprintf(reply, sizeof reply, "error bad screen or scale");
            } else {
                if (s->scale[index - 1] != scale) wlr_log(WLR_INFO, "screen %d: KWin scale %g", index, scale);
                s->scale[index - 1] = scale;
                snprintf(reply, sizeof reply, "ok");
            }
        } else if (sscanf(buf, "key %u %d", &code, &value) == 2) {
            handle_key(s, code, value, reply, sizeof reply);
            len = sizeof from;
            continue;  // no reply: keys are fire-and-forget
        } else if (strncmp(buf, "vrkeyboard ", 11) == 0) {
            keyboard_command(s, buf + 11, reply, sizeof reply);
        } else if (strncmp(buf, "input ", 6) == 0) {
            char what[8] = "", button[8] = "left";
            double x = 0, y = 0;
            const int got = sscanf(buf, "input %d %7s %lf %lf %7s", &index, what, &x, &y, button);
            struct ft_event e = {.screen = index - 1, .x = x, .y = y, .button = BTN_LEFT};
            if (strcmp(button, "right") == 0) e.button = BTN_RIGHT;
            else if (strcmp(button, "middle") == 0) e.button = BTN_MIDDLE;
            if (got >= 2 && strcmp(what, "leave") == 0) e.type = FT_LEAVE;
            else if (got >= 4 && strcmp(what, "move") == 0) e.type = FT_MOTION;
            else if (got >= 4 && (strcmp(what, "down") == 0 || strcmp(what, "up") == 0))
                e.type = FT_BUTTON, e.pressed = what[0] == 'd';
            else index = 0;
            if (index < 1 || index > MAX_SCREENS || !s->screens[index - 1]) {
                snprintf(reply, sizeof reply, "error input <screen> move|down|up|leave [x y [button]]");
            } else {
                handle_vr_event(&e, s);
                snprintf(reply, sizeof reply, "ok");
            }
        } else if (strcmp(buf, "toplevels") == 0) {
            int n = 0, at = 0;
            for (int i = 0; i < MAX_SCREENS; ++i) n += s->screens[i] != NULL;
            at = snprintf(reply, sizeof reply, "ok %d", n);
            for (int i = 0; i < MAX_SCREENS && at < (int)sizeof reply; ++i) {
                const struct screen *sc = s->screens[i];
                if (!sc) continue;
                at += snprintf(reply + at, sizeof reply - at, "\n%d %dx%d %u %s", i + 1, sc->buffer_width,
                               sc->buffer_height, sc->commits, sc->toplevel->title ? sc->toplevel->title : "");
            }
        } else if (strncmp(buf, "click ", 6) == 0) {
            // A click on another panel takes typing to Steam. Our own panels (the screens,
            // their controls) leave it: a click on a screen arrives as a panel event.
            if (strcmp(buf + 6, "-") != 0 && strncmp(buf + 6, "frametop.", 9) != 0) s->keys_clicked = false;
            len = sizeof from;
            continue;
        } else {
            ft_vr_command(buf, reply, sizeof reply);
        }
        if (len > offsetof(struct sockaddr_un, sun_path))
            sendto(fd, reply, strlen(reply), MSG_DONTWAIT, (struct sockaddr *)&from, len);
        len = sizeof from;
    }
    return 0;
}

static int open_control_socket(const char *name) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    const size_t len = strlen(name);
    if (len == 0 || len >= sizeof addr.sun_path - 1) {
        wlr_log(WLR_ERROR, "bad control socket name");
        close(fd);
        return -1;
    }
    memcpy(addr.sun_path + 1, name, len);
    if (bind(fd, (struct sockaddr *)&addr, offsetof(struct sockaddr_un, sun_path) + 1 + len) != 0) {
        wlr_log(WLR_ERROR, "can't bind @%s (another ft-screens running?)", name);
        close(fd);
        return -1;
    }
    return fd;
}

static int stop(int sig, void *data) {
    wl_display_terminate(((struct server *)data)->display);
    return 0;
}

// ---------------------------------------------------------------- setup

static void keyboard_led(struct wlr_keyboard *kb, uint32_t leds) {}
static const struct wlr_keyboard_impl keyboard_impl = {.name = "ft-screens-keyboard", .led_update = keyboard_led};

static bool setup_dmabuf(struct server *s) {
    struct stat st;
    const char *node = "/dev/dri/renderD128";
    if (stat(node, &st) != 0) {
        wlr_log(WLR_ERROR, "no %s", node);
        return false;
    }
    struct wlr_linux_dmabuf_feedback_v1 fb = {.main_device = st.st_rdev};
    wl_array_init(&fb.tranches);
    struct wlr_linux_dmabuf_feedback_v1_tranche *tr = wlr_linux_dmabuf_feedback_add_tranche(&fb);
    tr->target_device = st.st_rdev;
    const uint32_t formats[] = {DRM_FORMAT_XRGB8888, DRM_FORMAT_ARGB8888};
    for (size_t f = 0; f < 2; ++f) {
        uint64_t mods[64];
        const int n = ft_vr_modifiers(formats[f], mods, 64);
        for (int i = 0; i < n; ++i) wlr_drm_format_set_add(&tr->formats, formats[f], mods[i]);
        wlr_log(WLR_INFO, "dmabuf: format 0x%x, %d modifiers SteamVR can import", formats[f], n);
    }
    struct wlr_linux_dmabuf_v1 *dmabuf = wlr_linux_dmabuf_v1_create(s->display, 4, &fb);
    wlr_linux_dmabuf_feedback_v1_finish(&fb);
    return dmabuf != NULL;
}

int main(int argc, char **argv) {
    struct server s = {0};
    ft_mouse_init(&s.mouse);
    for (int i = 0; i < MAX_SCREENS; ++i) s.scale[i] = 1;
    s.kb_screen = -1;
    const char *socket_name = "ft-screens-0", *control_name = "ft_screens";
    char **command = NULL;
    s.vr = true;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_name = argv[++i];
        } else if (strcmp(argv[i], "--control") == 0 && i + 1 < argc) {
            control_name = argv[++i];
        } else if (strcmp(argv[i], "--spares") == 0 && i + 1 < argc) {
            s.spares = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-vr") == 0) {
            s.vr = false;
        } else if (strcmp(argv[i], "--screen") == 0 && i + 1 < argc && s.n_config < MAX_SCREENS) {
            struct config *c = &s.config[s.n_config];
            c->metres = 0;
            if (sscanf(argv[++i], "%dx%d@%lf", &c->width, &c->height, &c->metres) < 2) {
                fprintf(stderr, "bad --screen %s (want WxH@METRES)\n", argv[i]);
                return 2;
            }
            if (c->metres <= 0) c->metres = 1.5 * c->width / 1920.0;
            ++s.n_config;
        } else if (strcmp(argv[i], "--") == 0) {
            command = &argv[i + 1];
            break;
        } else {
            fprintf(stderr,
                    "usage: %s [--socket NAME] [--control NAME] [--no-vr] [--screen WxH@METRES]... "
                    "[--spares N] [-- COMMAND ARGS...]\n",
                    argv[0]);
            return 2;
        }
    }
    if (s.n_config == 0) s.config[s.n_config++] = (struct config){3440, 1440, 2.4};

    setvbuf(stdout, NULL, _IOLBF, 0);  // vr.cpp prints to stdout; keep it in order with the log
    wlr_log_init(WLR_INFO, NULL);
    if (s.vr && !ft_vr_init()) return 1;
    if (!s.vr) wlr_log(WLR_INFO, "--no-vr: running without SteamVR");

    s.display = wl_display_create();
    s.loop = wl_display_get_event_loop(s.display);
    wl_list_init(&s.buffers);
    struct wlr_compositor *compositor = wlr_compositor_create(s.display, 6, NULL);
    ft_cursor_cache_init(&s.cursor_cache, compositor);
    wlr_subcompositor_create(s.display);
    const uint32_t shm_formats[] = {DRM_FORMAT_ARGB8888, DRM_FORMAT_XRGB8888};  // wlroots wants DRM codes
    wlr_shm_create(s.display, 1, shm_formats, 2);
    if (!setup_dmabuf(&s)) return 1;
    wlr_data_device_manager_create(s.display);

    s.xdg_shell = wlr_xdg_shell_create(s.display, 3);
    s.new_toplevel.notify = new_toplevel;
    wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_toplevel);
    s.decoration = wlr_xdg_decoration_manager_v1_create(s.display);
    s.new_decoration.notify = new_decoration;
    wl_signal_add(&s.decoration->events.new_toplevel_decoration, &s.new_decoration);

    s.seat = wlr_seat_create(s.display, "seat0");
    s.request_cursor.notify = cursor_requested;
    wl_signal_add(&s.seat->events.request_set_cursor, &s.request_cursor);
    wlr_keyboard_init(&s.keyboard, &keyboard_impl, "ft-screens-keyboard");
    struct xkb_context *xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *keymap = xkb_keymap_new_from_names(xkb, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
    wlr_keyboard_set_keymap(&s.keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(xkb);
    wlr_seat_set_keyboard(s.seat, &s.keyboard);
    wlr_seat_set_capabilities(s.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);

    if (wl_display_add_socket(s.display, socket_name) != 0) {
        wlr_log(WLR_ERROR, "can't create socket %s (another ft-screens?)", socket_name);
        return 1;
    }
    char path[256];
    snprintf(path, sizeof path, "%s/%s", getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp", socket_name);
    wlr_log(WLR_INFO, "listening on %s; %d screen(s) configured, %d spare(s)", path, s.n_config, s.spares);

    const int control = open_control_socket(control_name);
    if (control < 0) return 1;
    s.relay_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    wl_event_loop_add_fd(s.loop, control, WL_EVENT_READABLE, control_readable, &s);

    s.tick = wl_event_loop_add_timer(s.loop, tick, &s);
    wl_event_source_timer_update(s.tick, 11);
    wl_event_loop_add_signal(s.loop, SIGINT, stop, &s);
    wl_event_loop_add_signal(s.loop, SIGTERM, stop, &s);
    wl_event_loop_add_signal(s.loop, SIGCHLD, child_exited, &s);

    if (command && command[0]) {
        s.child = fork();
        if (s.child == 0) {
            setenv("WAYLAND_DISPLAY", path, 1);
            execvp(command[0], command);
            perror(command[0]);
            _exit(127);
        }
    }

    wl_display_run(s.display);

    wlr_log(WLR_INFO, "stopping");
    if (s.child > 0) kill(s.child, SIGTERM);
    wl_display_destroy_clients(s.display);
    // wlroots asserts that nothing still listens to its globals when they go.
    wl_list_remove(&s.new_toplevel.link);
    wl_list_remove(&s.new_decoration.link);
    ft_cursor_cache_finish(&s.cursor_cache);
    ft_vr_shutdown();
    wl_display_destroy(s.display);
    return 0;
}
