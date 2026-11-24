/* Значки хаба: контуры SVG Lucide (данные — icons_data.c, создаёт scripts/icons.py). */
#ifndef WASHI_UI_ICONS_DATA_H
#define WASHI_UI_ICONS_DATA_H

#include "gfx.h"

typedef struct {
    float vb_x, vb_y, vb_w, vb_h; /* viewBox */
    const char *d;                /* контур SVG (path d) */
} icon_svg;

/* Lucide — линии (stroke-width 2 на 24). */
extern const icon_svg icons_lucide[ICON_COUNT];

#endif
