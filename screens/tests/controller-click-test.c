#include <assert.h>
#include "../controller-click.h"
static struct ft_event event(enum ft_event_type t, bool down, double x, double y) {
    return (struct ft_event){.type=t,.screen=0,.button=BTN_LEFT,.pressed=down,.x=x,.y=y};
}
int main(void) {
    struct ft_controller_click c={.threshold=8};
    struct ft_event e=event(FT_BUTTON,true,100,100);
    assert(ft_controller_click_filter(&c,&e,2));
    e=event(FT_MOTION,false,112,108);
    assert(!ft_controller_click_filter(&c,&e,2));
    assert(c.held && !c.dragging && c.suppressed==1);
    e=event(FT_BUTTON,false,116,111);
    assert(ft_controller_click_filter(&c,&e,2));
    assert(e.x==100 && e.y==100 && c.clicks==1 && !c.held);
    e=event(FT_BUTTON,true,100,100);ft_controller_click_filter(&c,&e,1);
    e=event(FT_MOTION,false,109,100);assert(ft_controller_click_filter(&c,&e,1));
    assert(c.dragging && c.drags==1);
    e=event(FT_MOTION,false,101,100);assert(ft_controller_click_filter(&c,&e,1));
    e=event(FT_BUTTON,false,103,100);assert(ft_controller_click_filter(&c,&e,1));
    assert(e.x==103 && c.clicks==1 && !c.held);
    e=event(FT_BUTTON,true,100,100);ft_controller_click_filter(&c,&e,1);
    e=event(FT_LEAVE,false,0,0);assert(!ft_controller_click_filter(&c,&e,1));
    e=event(FT_BUTTON,false,800,900);e.screen=1;ft_controller_click_filter(&c,&e,1);
    assert(e.screen==0 && e.x==100 && c.clicks==2);
    e=event(FT_BUTTON,true,100,100);ft_controller_click_filter(&c,&e,1);
    e=event(FT_MOTION,false,101,100);e.screen=1;
    assert(ft_controller_click_filter(&c,&e,1) && c.dragging);
    e=event(FT_BUTTON,false,101,100);ft_controller_click_filter(&c,&e,1);
    c.threshold=0;
    e=event(FT_BUTTON,true,100,100);ft_controller_click_filter(&c,&e,1);
    e=event(FT_MOTION,false,101,100);assert(ft_controller_click_filter(&c,&e,1) && !c.held);
    c.threshold=8;
    e=event(FT_BUTTON,true,100,100);ft_controller_click_filter(&c,&e,1);
    e=event(FT_BUTTON,true,100,100);e.button=BTN_RIGHT;ft_controller_click_filter(&c,&e,1);
    assert(!c.held);
}
