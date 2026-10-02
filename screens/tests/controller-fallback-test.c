#include <assert.h>
#include <stdio.h>
#include "../controller-fallback.h"
int main(void) {
    struct ft_controller_fallback f = {0};
    assert(!ft_fallback_active(&f, 100));
    ft_fallback_presence(&f, false, true, 100);
    assert(!ft_fallback_active(&f, 1099));
    assert(ft_fallback_active(&f, 1100));
    ft_fallback_presence(&f, false, true, 1100);
    assert(ft_fallback_active(&f, 1100)); // heartbeat doesn't reset debounce
    assert(!ft_fallback_active(&f, 4100)); // lost relay fails closed
    ft_fallback_presence(&f, false, true, 4100);
    assert(!ft_fallback_active(&f, 4100)); // old absence cannot bypass new debounce
    assert(ft_fallback_active(&f, 5100));
    ft_fallback_presence(&f, true, true, 4200);
    assert(!ft_fallback_active(&f, 9000)); // connected but idle is still a mouse
    ft_fallback_presence(&f, false, false, 9100);
    assert(!ft_fallback_active(&f, 10100)); // disabled preference
    ft_fallback_presence(&f, true, true, 10200);
    ft_fallback_presence(&f, false, true, 10300);
    assert(!ft_fallback_active(&f, 11299));
    assert(ft_fallback_active(&f, 11300));
    ft_fallback_presence(&f, true, true, 11301);
    assert(!ft_fallback_active(&f, 11301)); // reconnect wins immediately
    ft_fallback_presence(&f, false, true, UINT32_MAX-500);
    assert(ft_fallback_active(&f, 500)); // monotonic counter wrap
    puts("disconnect debounce, heartbeat expiry, reconnect and disabled fallback passed");
}
