#include "gesture.h"

#include <stdlib.h>

gesture gesture_feed(gesture_state *st, const pf_event *ev)
{
    gesture g = { GESTURE_NONE, 0, 0, 0, 0 };
    switch (ev->type) {
    case PF_EVENT_POINTER_DOWN:
        st->down = 1;
        st->long_fired = 0;
        st->x0 = ev->x;
        st->y0 = ev->y;
        break;
    case PF_EVENT_POINTER_LONG:
        if (st->down && !st->long_fired) {
            st->long_fired = 1;
            g.kind = GESTURE_LONG;
            g.x = g.x1 = st->x0;
            g.y = g.y1 = st->y0;
        }
        break;
    case PF_EVENT_POINTER_UP: {
        if (!st->down)
            break;
        st->down = 0;
        if (st->long_fired)
            break;
        int dx = ev->x - st->x0, dy = ev->y - st->y0;
        g.x = st->x0;
        g.y = st->y0;
        g.x1 = ev->x;
        g.y1 = ev->y;
        if (abs(dx) < GESTURE_SWIPE_MIN && abs(dy) < GESTURE_SWIPE_MIN)
            g.kind = GESTURE_TAP;
        else if (abs(dx) >= abs(dy))
            g.kind = dx < 0 ? GESTURE_SWIPE_LEFT : GESTURE_SWIPE_RIGHT;
        else
            g.kind = dy < 0 ? GESTURE_SWIPE_UP : GESTURE_SWIPE_DOWN;
        break;
    }
    default:
        break;
    }
    return g;
}

const char *gesture_name(gesture_kind k)
{
    switch (k) {
    case GESTURE_TAP: return "касание";
    case GESTURE_LONG: return "долгое касание";
    case GESTURE_SWIPE_LEFT: return "свайп влево";
    case GESTURE_SWIPE_RIGHT: return "свайп вправо";
    case GESTURE_SWIPE_UP: return "свайп вверх";
    case GESTURE_SWIPE_DOWN: return "свайп вниз";
    default: return "нет";
    }
}
