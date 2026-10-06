#include <assert.h>
#include "../relay-buttons.h"
int main(void) {
    // What goes where: keys to the keyboard, mouse buttons to the pointer, the rest nowhere.
    assert(ft_relay_key_kind(KEY_A) == FT_RELAY_KEY && ft_relay_key_kind(KEY_PLAYPAUSE) == FT_RELAY_KEY);
    assert(ft_relay_key_kind(BTN_LEFT) == FT_RELAY_BUTTON && ft_relay_key_kind(BTN_SIDE) == FT_RELAY_BUTTON);
    assert(ft_relay_key_kind(BTN_EXTRA) == FT_RELAY_BUTTON && ft_relay_key_kind(BTN_TASK) == FT_RELAY_BUTTON);
    for (uint32_t c = BTN_MISC; c < BTN_MOUSE; ++c) assert(ft_relay_key_kind(c) == FT_RELAY_DROP);
    for (uint32_t c = BTN_TASK + 1; c < KEY_OK; ++c) assert(ft_relay_key_kind(c) == FT_RELAY_DROP);  // joystick, gamepad, digitizer
    assert(ft_relay_key_kind(KEY_OK) == FT_RELAY_KEY && ft_relay_key_kind(BTN_TRIGGER_HAPPY - 1) == FT_RELAY_KEY);
    assert(ft_relay_key_kind(BTN_TRIGGER_HAPPY) == FT_RELAY_DROP && ft_relay_key_kind(KEY_MAX) == FT_RELAY_DROP);
    assert(ft_relay_key_kind(KEY_MAX + 1) == FT_RELAY_DROP);

    struct ft_relay_buttons b = {0};
    // No pointer on a visible screen (or paused): the press is dropped, and so is its release.
    assert(!ft_relay_button(&b, BTN_SIDE, true, false) && !b.held);
    assert(!ft_relay_button(&b, BTN_SIDE, false, true));
    // A release without a press is nothing, whatever the pointer does.
    assert(!ft_relay_button(&b, BTN_EXTRA, false, false) && !ft_relay_button(&b, BTN_EXTRA, false, true));
    // A press on a screen, then typing moves to Steam or the pointer leaves: the release still goes.
    assert(ft_relay_button(&b, BTN_SIDE, true, true) && b.held == 1u << (BTN_SIDE - BTN_MOUSE));
    assert(!ft_relay_button(&b, BTN_SIDE, true, true));  // a second press of a held button
    assert(ft_relay_button(&b, BTN_SIDE, false, false) && !b.held);
    assert(!ft_relay_button(&b, BTN_SIDE, false, false));  // once
    // Not a mouse button: never the pointer's.
    assert(!ft_relay_button(&b, KEY_A, true, true) && !ft_relay_button(&b, BTN_JOYSTICK, true, true));
    assert(!ft_relay_button(&b, BTN_TRIGGER_HAPPY, true, true) && !b.held);

    // ft-screens lets go itself (the pointer left, its screen hid, a pause): each held button
    // once, then the relay's own releases find nothing held.
    assert(ft_relay_button(&b, BTN_LEFT, true, true) && ft_relay_button(&b, BTN_TASK, true, true));
    assert(ft_relay_buttons_take(&b) == BTN_LEFT && ft_relay_buttons_take(&b) == BTN_TASK);
    assert(ft_relay_buttons_take(&b) == 0 && !b.held);
    assert(!ft_relay_button(&b, BTN_LEFT, false, true) && !ft_relay_button(&b, BTN_TASK, false, true));
    // ...and the next press goes through again.
    assert(ft_relay_button(&b, BTN_LEFT, true, true) && ft_relay_button(&b, BTN_LEFT, false, true));
}
