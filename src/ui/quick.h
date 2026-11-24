/* quick — быстрые настройки хаба: шторка сверху (свайп вниз от строки состояния или касание
 * её): подсветка (яркость, теплота — ползунки), Wi‑Fi, очистить экран. */
#ifndef WASHI_UI_QUICK_H
#define WASHI_UI_QUICK_H

#include "../platform/platform.h"
#include "gfx.h"

typedef struct quick quick;

typedef enum {
    QUICK_NONE,
    QUICK_CLOSE, /* закрыть шторку (перерисовать экран под ней) */
    QUICK_CLEAN, /* закрыть и полностью перерисовать экран (убрать следы) */
} quick_action;

quick *quick_new(gfx *g);
void quick_free(quick *q);

/* Нарисовать шторку поверх экрана и обновить её область. */
void quick_draw(quick *q, pf_screen *s);

/* Событие указателя или кнопки, пока шторка открыта. */
quick_action quick_event(quick *q, const pf_event *ev);

#endif
