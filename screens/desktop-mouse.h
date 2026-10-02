#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#define FT_MOUSE_OUTPUTS 24
struct ft_mouse_output { bool enabled; double x, y, width, height, scale; };
struct ft_mouse {
    struct ft_mouse_output outputs[FT_MOUSE_OUTPUTS];
    double x, y;
    uint32_t buttons;
    int screen, grab;
    bool positioned;
};
static inline void ft_mouse_init(struct ft_mouse *m) { *m = (struct ft_mouse){.screen = -1, .grab = -1}; }
static inline bool ft_mouse_output_set(struct ft_mouse *m, int i, double x, double y,
                                      double w, double h, double scale) {
    if (i < 0 || i >= FT_MOUSE_OUTPUTS || m->buttons || !isfinite(x) || !isfinite(y) ||
        !isfinite(w) || !isfinite(h) || !isfinite(scale) || fabs(x) > 65536 || fabs(y) > 65536 ||
        w < 1 || h < 1 || w > 16384 || h > 16384 || scale < .25 || scale > 8) return false;
    m->outputs[i] = (struct ft_mouse_output){true, x, y, w, h, scale};
    return true;
}
static inline bool ft_mouse_position(struct ft_mouse *m, double x, double y) {
    if (!isfinite(x) || !isfinite(y)) return false;
    double best = INFINITY, bx = 0, by = 0;
    int screen = -1;
    for (int i = 0; i < FT_MOUSE_OUTPUTS; ++i) {
        const struct ft_mouse_output *o = &m->outputs[i];
        if (!o->enabled) continue;
        double px = fmin(fmax(x, o->x), o->x + o->width - 1);
        double py = fmin(fmax(y, o->y), o->y + o->height - 1);
        double d = (px-x)*(px-x) + (py-y)*(py-y);
        if (d < best) best = d, bx = px, by = py, screen = i;
    }
    if (screen < 0) return false;
    m->x = bx; m->y = by; m->screen = screen; m->positioned = true;
    return true;
}
static inline bool ft_mouse_move(struct ft_mouse *m, double dx, double dy) {
    if (!isfinite(dx) || !isfinite(dy) || fabs(dx) > 16384 || fabs(dy) > 16384) return false;
    if (!m->positioned) {
        for (int i = 0; i < FT_MOUSE_OUTPUTS; ++i) if (m->outputs[i].enabled) {
            const struct ft_mouse_output *o = &m->outputs[i];
            if (!ft_mouse_position(m, o->x + o->width/2, o->y + o->height/2)) return false;
            break;
        }
    }
    return ft_mouse_position(m, m->x + dx, m->y + dy);
}
// The host seat's implicit grab stays on one KWin output during a drag. Motion
// remains in that output's coordinates, including positions beyond its edges;
// nested KWin adds the output origin and can drag windows across its monitors.
static inline int ft_mouse_target(const struct ft_mouse *m) { return m->buttons ? m->grab : m->screen; }
static inline bool ft_mouse_button(struct ft_mouse *m, unsigned button, bool down) {
    if (!m->positioned || button < 272 || button > 279) return false;
    uint32_t bit = 1u << (button - 272);
    if (down) { if (!m->buttons) m->grab = m->screen; m->buttons |= bit; }
    else { m->buttons &= ~bit; if (!m->buttons) m->grab = -1; }
    return true;
}
