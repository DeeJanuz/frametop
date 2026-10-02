#ifndef FT_DESKTOP_HANDOFF_H
#define FT_DESKTOP_HANDOFF_H
#include <stdbool.h>
#include <stdint.h>
enum ft_handoff_policy { FT_PREFER_MOUSE, FT_LAST_ACTIVE, FT_PREFER_POINTER };
struct ft_handoff { enum ft_handoff_policy policy; bool mouse_seen; uint32_t mouse_at; };
// Only deliberate desktop actions call this. Tracking motion never claims a seat.
static inline bool ft_handoff_claim(struct ft_handoff *h, bool pointer,
                                    uint32_t now, uint32_t mouse_buttons,
                                    uint32_t pointer_buttons) {
    if (pointer) {
        if (mouse_buttons || h->policy == FT_PREFER_MOUSE) return false;
        if (h->policy == FT_LAST_ACTIVE && !pointer_buttons && h->mouse_seen && now == h->mouse_at)
            return false; // Same millisecond: native mouse wins.
    } else {
        if (pointer_buttons || h->policy == FT_PREFER_POINTER) return false;
        h->mouse_seen = true; h->mouse_at = now;
    }
    return true;
}
static inline const char *ft_handoff_name(enum ft_handoff_policy p) {
    return p == FT_LAST_ACTIVE ? "last-active" : p == FT_PREFER_POINTER ? "pointer" : "mouse";
}
#endif
