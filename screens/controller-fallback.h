#ifndef FT_CONTROLLER_FALLBACK_H
#define FT_CONTROLLER_FALLBACK_H
#include <stdbool.h>
#include <stdint.h>
struct ft_controller_fallback {
    bool known, present, enabled;
    uint32_t received, absent_since;
};
static inline void ft_fallback_presence(struct ft_controller_fallback *f,
                                       bool present, bool enabled, uint32_t now) {
    if (!f->known || (uint32_t)(now-f->received) >= 3000 || f->present || present)
        f->absent_since = now;
    f->known = true; f->present = present; f->enabled = enabled; f->received = now;
}
static inline bool ft_fallback_active(const struct ft_controller_fallback *f, uint32_t now) {
    return f->known && f->enabled && !f->present &&
        (uint32_t)(now-f->received) < 3000 && (uint32_t)(now-f->absent_since) >= 1000;
}
#endif
