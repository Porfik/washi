/* Текст интерфейса прежним вызовом (обычное начертание): обёртка над gfx.h.
 * Только для потока интерфейса. */
#ifndef WASHI_UI_TEXT_H
#define WASHI_UI_TEXT_H

#include <stdint.h>

#include "gfx.h"

typedef gfx ui_text;

ui_text *ui_text_new(void);
void ui_text_free(ui_text *t);

/* Строка UTF-8 от (x, baseline) кеглем size пикселей; возвращает ширину в пикселях. */
int ui_text_draw(ui_text *t, pf_screen *s, int x, int baseline, float size, uint8_t gray,
                 const char *str);

/* То же с переносом по словам в ширину max_w; возвращает baseline следующей строки. */
int ui_text_draw_wrapped(ui_text *t, pf_screen *s, int x, int baseline, int max_w, float size,
                         uint8_t gray, const char *str);

/* Ширина строки в пикселях. */
int ui_text_width(ui_text *t, float size, const char *str);

/* Строка в ширину max_w: не влезает — обрезается с «…». Возвращает ширину. */
int ui_text_draw_fit(ui_text *t, pf_screen *s, int x, int baseline, int max_w, float size, uint8_t gray,
                     const char *str);

void ui_fill(pf_screen *s, int x, int y, int w, int h, uint8_t gray);

#endif
