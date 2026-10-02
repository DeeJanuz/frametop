#include "../desktop-mouse.h"
#include <assert.h>
#include <stdio.h>
int main(void) {
    struct ft_mouse m;
    ft_mouse_init(&m);
    assert(!ft_mouse_move(&m, 0, 0));
    assert(ft_mouse_output_set(&m, 0, -1280, 0, 1280, 720, 1.5));
    assert(ft_mouse_output_set(&m, 1, 0, 0, 1920, 1080, 1));
    assert(ft_mouse_move(&m, 0, 0));
    assert(m.screen == 0 && m.x == -640 && m.y == 360);
    assert(ft_mouse_position(&m, -1, 100));
    assert(ft_mouse_button(&m, 272, true));
    assert(!ft_mouse_output_set(&m, 0, 0, 0, 100, 100, 1));
    assert(ft_mouse_move(&m, 20, 0));
    assert(m.screen == 1 && m.x == 19 && ft_mouse_target(&m) == 0);
    // During the grab, host coordinates exceed the old output's width.
    assert((m.x-m.outputs[0].x)*m.outputs[0].scale == 1948.5);
    assert(ft_mouse_button(&m, 275, true));
    assert(ft_mouse_button(&m, 272, false));
    assert(ft_mouse_target(&m) == 0);
    assert(ft_mouse_button(&m, 275, false));
    assert(ft_mouse_target(&m) == 1 && m.grab == -1);
    assert(ft_mouse_move(&m, 10000, 10000));
    assert(m.x == 1919 && m.y == 1079);
    assert(!ft_mouse_move(&m, NAN, 0));
    assert(!ft_mouse_position(&m, INFINITY, 1));
    assert(!ft_mouse_move(&m, 16385, 0));
    assert(!ft_mouse_output_set(&m, FT_MOUSE_OUTPUTS, 0, 0, 100, 100, 1));
    assert(!ft_mouse_output_set(&m, 0, 0, 0, 0, 100, 1));
    assert(!ft_mouse_output_set(&m, 0, 0, 0, 100, 100, NAN));
    assert(!ft_mouse_button(&m, 280, true));
    puts("desktop mouse geometry, drag and validation passed");
}
