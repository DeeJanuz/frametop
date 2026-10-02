#ifndef FT_CONTROLLER_CLICK_H
#define FT_CONTROLLER_CLICK_H
#include <math.h>
#include <linux/input-event-codes.h>
#include "vr.h"
struct ft_controller_click {
    bool held, dragging;
    int screen;
    uint32_t buttons;
    double x, y, threshold, radius;
    unsigned long suppressed, clicks, drags;
};
// Hold the desktop position at press time until movement exceeds a logical-pixel
// radius. No timer, delayed button-down, or change to native mouse input.
static inline bool ft_controller_click_filter(struct ft_controller_click *c,
                                              struct ft_event *e, double scale) {
    if (e->type == FT_BUTTON && e->button >= BTN_LEFT && e->button < BTN_LEFT+8) {
        uint32_t bit = 1u << (e->button-BTN_LEFT);
        if (e->pressed) c->buttons |= bit; else c->buttons &= ~bit;
    }
    if (e->type == FT_BUTTON && e->pressed) {
        if (e->button == BTN_LEFT && !c->held && c->threshold > 0) {
            c->held = true; c->dragging = false; c->screen = e->screen;
            c->x = e->x; c->y = e->y; c->radius = c->threshold * scale;
        } else if (e->button != BTN_LEFT) {
            c->held = false; // Multi-button gestures retain their usual semantics.
        }
    } else if (e->type == FT_MOTION && c->held && !c->dragging) {
        if (e->screen == c->screen && hypot(e->x-c->x, e->y-c->y) <= c->radius) {
            ++c->suppressed;
            return false;
        }
        c->dragging = true; ++c->drags;
    } else if (e->type == FT_BUTTON && e->button == BTN_LEFT && !e->pressed && c->held) {
        if (!c->dragging) {
            e->screen = c->screen; e->x = c->x; e->y = c->y;
            ++c->clicks;
        }
        c->held = false;
    } else if (e->type == FT_LEAVE && c->held) {
        return false; // Preserve the implicit grab through tiny edge excursions.
    }
    return true;
}
#endif
