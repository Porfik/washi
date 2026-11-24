/* gesture — касание, долгое касание и свайпы из событий указателя платформы.
 *
 * Касание — палец отпустили недалеко от места нажатия; свайп — сдвиг не меньше
 * GESTURE_SWIPE_MIN точек (≈ 8 мм), направление — по большей составляющей; долгое
 * касание — PF_EVENT_POINTER_LONG (отпускание после него ничего не даёт).
 */
#ifndef WASHI_UI_GESTURE_H
#define WASHI_UI_GESTURE_H

#include "../platform/platform.h"

#define GESTURE_SWIPE_MIN 100

typedef enum {
    GESTURE_NONE,
    GESTURE_TAP,
    GESTURE_LONG,
    GESTURE_SWIPE_LEFT,  /* палец ведут справа налево */
    GESTURE_SWIPE_RIGHT,
    GESTURE_SWIPE_UP,
    GESTURE_SWIPE_DOWN,
} gesture_kind;

typedef struct {
    gesture_kind kind;
    int x, y;   /* где коснулись */
    int x1, y1; /* где отпустили (для свайпа) */
} gesture;

typedef struct {
    int down;
    int long_fired;
    int x0, y0;
} gesture_state;

/* Передать событие; вернёт распознанный жест (GESTURE_NONE — пока ничего). */
gesture gesture_feed(gesture_state *st, const pf_event *ev);

const char *gesture_name(gesture_kind k);

#endif
