/* rotate — логический экран в альбомной ориентации поверх физического портретного.
 *
 * Код выше платформы рисует в логический буфер (в альбоме — 1872×1404); платформа переносит
 * обновляемые области в физический экран с поворотом и поворачивает координаты касаний.
 * orient: 0 — портрет; 1 — альбом, верх у правого края книги (повёрнута против часовой);
 * 2 — альбом, верх у левого края (по часовой).
 */
#ifndef WASHI_PLATFORM_ROTATE_H
#define WASHI_PLATFORM_ROTATE_H

#include "platform.h"

typedef struct {
    int orient;
    pf_screen logical; /* буфер для orient != 0 */
} rotator;

/* Сменить ориентацию; логический буфер заводится по размеру phys и заливается белым.
 * 0 — получилось. */
int rot_set(rotator *r, int orient, const pf_screen *phys);

/* Экран для рисования: phys в портрете, иначе логический. */
pf_screen *rot_screen(rotator *r, pf_screen *phys);

/* Перенести область (x, y, w, h) логического экрана в phys; out — та же область на phys. */
void rot_flush(const rotator *r, pf_screen *phys, int x, int y, int w, int h, int out[4]);

/* Координаты касания на phys → на логическом экране. */
void rot_touch(const rotator *r, const pf_screen *phys, int *x, int *y);

#endif
