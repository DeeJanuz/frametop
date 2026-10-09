#ifndef FT_RELAY_BUTTONS_H
#define FT_RELAY_BUTTONS_H
#include <stdbool.h>
#include <stdint.h>
#include <linux/input-event-codes.h>
// Keys from the input relay ("key <code> <value>" on the control socket). Keys go where
// typing goes (compositor.c, handle_key). Mouse buttons, a mouse's side buttons that pointer
// mode passes through as keys, go where the pointer is, like a laser's click: a press needs
// the pointer on a visible screen, nothing paused, and that button not held already, and a
// release goes whenever its press went, whatever changed since. When the pointer leaves the
// screens, or its screen hides or closes, or everything pauses, ft-screens releases the held
// ones itself (ft_relay_buttons_take), and the relay's releases later find nothing held.
// wlroots counts presses per button, so one release lost would swallow that button for good
// (a laser's left click too), and KWin would keep it held.
enum ft_relay_key { FT_RELAY_DROP, FT_RELAY_KEY, FT_RELAY_BUTTON };

// What a code from the relay is: a key for the keyboard (below BTN_MISC, and KEY_OK up to
// BTN_TRIGGER_HAPPY: xkeyboard-config names keycodes above 255 too), a mouse button for the
// pointer (BTN_MOUSE..BTN_TASK), or nothing to send (joystick, gamepad and digitizer
// buttons, BTN_TRIGGER_HAPPY and up).
static inline enum ft_relay_key ft_relay_key_kind(uint32_t code) {
    if (code < BTN_MISC || (code >= KEY_OK && code < BTN_TRIGGER_HAPPY)) return FT_RELAY_KEY;
    if (code >= BTN_MOUSE && code <= BTN_TASK) return FT_RELAY_BUTTON;
    return FT_RELAY_DROP;
}

struct ft_relay_buttons {
    uint8_t held;  // pressed on the seat and not released, a bit each (1 << (code - BTN_MOUSE))
};

// A relay button pressed or released: true if it goes to the seat. can_press: the pointer
// is on a visible screen, and nothing's paused.
static inline bool ft_relay_button(struct ft_relay_buttons *b, uint32_t code, bool pressed, bool can_press) {
    if (ft_relay_key_kind(code) != FT_RELAY_BUTTON) return false;
    const uint8_t bit = (uint8_t)(1u << (code - BTN_MOUSE));
    if (pressed) {
        if (!can_press || (b->held & bit)) return false;
        b->held |= bit;
        return true;
    }
    if (!(b->held & bit)) return false;
    b->held &= (uint8_t)~bit;
    return true;
}

// The next held button, no longer held here (0: none), for ft-screens to release itself.
static inline uint32_t ft_relay_buttons_take(struct ft_relay_buttons *b) {
    for (uint32_t i = 0; i <= BTN_TASK - BTN_MOUSE; ++i)
        if (b->held & (1u << i)) {
            b->held &= (uint8_t)~(1u << i);
            return BTN_MOUSE + i;
        }
    return 0;
}
#endif
