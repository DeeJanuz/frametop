#include <assert.h>
#include "../desktop-handoff.h"
int main(void) {
    struct ft_handoff h = {.policy=FT_LAST_ACTIVE};
    assert(ft_handoff_claim(&h, false, 100, 0, 0));
    assert(!ft_handoff_claim(&h, true, 100, 0, 0));
    assert(ft_handoff_claim(&h, true, 101, 0, 0));
    assert(!ft_handoff_claim(&h, true, 102, 1, 0));
    assert(!ft_handoff_claim(&h, false, 102, 0, 1));
    assert(ft_handoff_claim(&h, true, 102, 0, 1));
    assert(ft_handoff_claim(&h, false, 102, 1, 0));
    assert(ft_handoff_claim(&h, false, 103, 0, 0));
    h.policy=FT_PREFER_MOUSE;
    assert(!ft_handoff_claim(&h, true, 104, 0, 0));
    assert(ft_handoff_claim(&h, false, 104, 0, 0));
    h.policy=FT_PREFER_POINTER;
    assert(!ft_handoff_claim(&h, false, 105, 0, 0));
    assert(ft_handoff_claim(&h, true, 105, 0, 0));
    h=(struct ft_handoff){.policy=FT_LAST_ACTIVE};
    assert(ft_handoff_claim(&h, false, UINT32_MAX, 0, 0));
    assert(ft_handoff_claim(&h, true, 0, 0, 0));
}
