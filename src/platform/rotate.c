#include "rotate.h"

#include <stdlib.h>
#include <string.h>

int rot_set(rotator *r, int orient, const pf_screen *phys)
{
    if (orient < 0 || orient > 2)
        orient = 0;
    if (orient == 0) {
        free(r->logical.pixels);
        memset(&r->logical, 0, sizeof r->logical);
        r->orient = 0;
        return 0;
    }
    int w = phys->height, h = phys->width, stride = (w + 15) & ~15;
    if (!r->logical.pixels || r->logical.width != w || r->logical.height != h) {
        free(r->logical.pixels);
        r->logical.pixels = malloc((size_t)stride * h);
        if (!r->logical.pixels) {
            memset(&r->logical, 0, sizeof r->logical);
            r->orient = 0;
            return -1;
        }
        r->logical.width = w;
        r->logical.height = h;
        r->logical.stride = stride;
    }
    memset(r->logical.pixels, 255, (size_t)r->logical.stride * h);
    r->orient = orient;
    return 0;
}

pf_screen *rot_screen(rotator *r, pf_screen *phys)
{
    return r->orient && r->logical.pixels ? &r->logical : phys;
}

#define TILE 32

void rot_flush(const rotator *r, pf_screen *phys, int x, int y, int w, int h, int out[4])
{
    const pf_screen *L = &r->logical;
    if (!r->orient || !L->pixels) {
        out[0] = x, out[1] = y, out[2] = w, out[3] = h;
        return;
    }
    if (x < 0)
        w += x, x = 0;
    if (y < 0)
        h += y, y = 0;
    if (x + w > L->width)
        w = L->width - x;
    if (y + h > L->height)
        h = L->height - y;
    if (w <= 0 || h <= 0) {
        out[0] = out[1] = out[2] = out[3] = 0;
        return;
    }
    int PW = phys->width, PH = phys->height;
    /* Плитками: в одной плитке и строки источника, и строки приёмника рядом в кэше. */
    for (int ty = y; ty < y + h; ty += TILE)
        for (int tx = x; tx < x + w; tx += TILE) {
            int ey = ty + TILE < y + h ? ty + TILE : y + h, ex = tx + TILE < x + w ? tx + TILE : x + w;
            for (int ly = ty; ly < ey; ly++) {
                const uint8_t *src = L->pixels + (size_t)ly * L->stride;
                if (r->orient == 1) {
                    /* (lx, ly) → (PW-1-ly, lx) */
                    uint8_t *dst = phys->pixels + (PW - 1 - ly);
                    for (int lx = tx; lx < ex; lx++)
                        dst[(size_t)lx * phys->stride] = src[lx];
                } else {
                    /* (lx, ly) → (ly, PH-1-lx) */
                    uint8_t *dst = phys->pixels + ly;
                    for (int lx = tx; lx < ex; lx++)
                        dst[(size_t)(PH - 1 - lx) * phys->stride] = src[lx];
                }
            }
        }
    if (r->orient == 1) {
        out[0] = PW - y - h, out[1] = x, out[2] = h, out[3] = w;
    } else {
        out[0] = y, out[1] = PH - x - w, out[2] = h, out[3] = w;
    }
}

void rot_touch(const rotator *r, const pf_screen *phys, int *x, int *y)
{
    int px = *x, py = *y;
    if (r->orient == 1) {
        *x = py;
        *y = phys->width - 1 - px;
    } else if (r->orient == 2) {
        *x = phys->height - 1 - py;
        *y = px;
    }
}
