#include "quick.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/clock.h"
#include "../base/log.h"
#include "widgets.h"

#define ROW_H 140
#define TILE_H 150
#define DRAG_REDRAW_MS 150

enum { HIT_NONE, HIT_BRIGHT, HIT_WARM, HIT_LIGHT, HIT_WIFI, HIT_CLEAN };

struct quick {
    gfx *g;
    int height;
    int light, bright, warm; /* есть ли подсветка; яркость; теплота (-1 — нет) */
    int wifi;                /* -1 — неизвестно */
    ui_rect bright_r, warm_r, tiles[3];
    int tile_hit[3], ntiles; /* что делает каждая плитка (HIT_LIGHT / HIT_WIFI / HIT_CLEAN) */
    int dragging; /* HIT_BRIGHT / HIT_WARM */
    double drawn;
    int down_x, down_y;
};

quick *quick_new(gfx *g)
{
    quick *q = calloc(1, sizeof *q);
    if (q)
        q->g = g;
    return q;
}

void quick_free(quick *q)
{
    free(q);
}

static void slider_row(quick *q, pf_screen *s, int y, const char *label, int value, ui_rect *hit)
{
    ui_slider_row(q->g, s, y, ROW_H, label, value);
    *hit = (ui_rect){ 0, y + 56, s->width, ROW_H - 40 };
}

/* Плитка-переключатель: включённая — чёрная, выключенная — светлая. */
static void tile(quick *q, pf_screen *s, ui_rect r, gfx_icon_id icon, const char *label, int on)
{
    gfx_rrect(q->g, s, r.x, r.y, r.w, r.h, UI_CARD_R, on ? UI_BLACK : UI_FAINT);
    uint8_t c = on ? UI_WHITE : UI_BLACK;
    gfx_icon_ex(q->g, s, icon, r.x + (r.w - 60) / 2, r.y + 22, 60, 0, on, c);
    gfx_style st = { FONT_MEDIUM, 30, c };
    int tw = gfx_text_width(q->g, st, label);
    gfx_text_fit(q->g, s, r.x + (r.w - (tw < r.w - 20 ? tw : r.w - 20)) / 2, r.y + 124, r.w - 20, st, label);
}

void quick_draw(quick *q, pf_screen *s)
{
    q->light = pf_light_get(&q->bright, &q->warm) == 0;
    q->wifi = pf_wifi_get();
    int y = UI_STATUS_H + 10;
    q->height = y + (q->light ? ROW_H : 0) + (q->light && q->warm >= 0 ? ROW_H : 0) + 30 + TILE_H + 30;
    gfx_fill(s, 0, 0, s->width, q->height, UI_WHITE);
    ui_status(q->g, s);
    memset(&q->bright_r, 0, sizeof q->bright_r);
    memset(&q->warm_r, 0, sizeof q->warm_r);
    if (q->light) {
        slider_row(q, s, y, "Подсветка", q->bright, &q->bright_r);
        y += ROW_H;
        if (q->warm >= 0) {
            slider_row(q, s, y, "Теплота", q->warm, &q->warm_r);
            y += ROW_H;
        }
    }
    y += 30;
    /* Плитки, которые не на что нажать (нет подсветки, Wi‑Fi неизвестен), не рисуем. */
    q->ntiles = 0;
    if (q->light)
        q->tile_hit[q->ntiles++] = HIT_LIGHT;
    if (q->wifi >= 0)
        q->tile_hit[q->ntiles++] = HIT_WIFI;
    q->tile_hit[q->ntiles++] = HIT_CLEAN;
    int gap = 24, tw = (s->width - 2 * UI_MARGIN - (q->ntiles - 1) * gap) / q->ntiles;
    for (int i = 0; i < q->ntiles; i++) {
        q->tiles[i] = (ui_rect){ UI_MARGIN + i * (tw + gap), y, tw, TILE_H };
        switch (q->tile_hit[i]) {
        case HIT_LIGHT:
            tile(q, s, q->tiles[i], ICON_SUN, q->bright > 0 ? "Свет вкл." : "Свет выкл.", q->bright > 0);
            break;
        case HIT_WIFI:
            tile(q, s, q->tiles[i], q->wifi ? ICON_WIFI : ICON_WIFI_OFF, q->wifi ? "Wi‑Fi вкл." : "Wi‑Fi выкл.",
                 q->wifi);
            break;
        default:
            tile(q, s, q->tiles[i], ICON_FIT_PAGE, "Очистить", 0);
        }
    }
    gfx_fill(s, 0, q->height - 2, s->width, 2, UI_TEXT3);
    pf_update(0, 0, s->width, q->height, PF_UPDATE_PARTIAL);
}

static int hit(const quick *q, int x, int y)
{
    if (ui_inside(q->bright_r, x, y))
        return HIT_BRIGHT;
    if (ui_inside(q->warm_r, x, y))
        return HIT_WARM;
    for (int i = 0; i < q->ntiles; i++)
        if (ui_inside(q->tiles[i], x, y))
            return q->tile_hit[i];
    return HIT_NONE;
}

static void set_slider(quick *q, pf_screen *s, int which, int x, int redraw)
{
    int v = ui_slider_percent(s, x);
    if (which == HIT_BRIGHT)
        q->bright = v;
    else
        q->warm = v;
    pf_light_set(q->bright, q->warm);
    if (redraw) {
        ui_rect r = which == HIT_BRIGHT ? q->bright_r : q->warm_r;
        int y = r.y - 56;
        slider_row(q, s, y, which == HIT_BRIGHT ? "Подсветка" : "Теплота", which == HIT_BRIGHT ? q->bright : q->warm,
                   which == HIT_BRIGHT ? &q->bright_r : &q->warm_r);
        pf_update(0, y, s->width, ROW_H, PF_UPDATE_FAST_BW);
    }
}

quick_action quick_event(quick *q, const pf_event *ev)
{
    pf_screen *s = pf_get_screen();
    if (!s)
        return QUICK_CLOSE;
    switch (ev->type) {
    case PF_EVENT_KEY_DOWN:
        return QUICK_CLOSE;
    case PF_EVENT_POINTER_DOWN:
        q->down_x = ev->x;
        q->down_y = ev->y;
        q->dragging = hit(q, ev->x, ev->y);
        if (q->dragging == HIT_BRIGHT || q->dragging == HIT_WARM) {
            set_slider(q, s, q->dragging, ev->x, 1);
            q->drawn = clock_ms();
        } else {
            q->dragging = HIT_NONE;
        }
        return QUICK_NONE;
    case PF_EVENT_POINTER_MOVE:
        if (q->dragging && clock_ms() - q->drawn >= DRAG_REDRAW_MS) {
            set_slider(q, s, q->dragging, ev->x, 1);
            q->drawn = clock_ms();
        }
        return QUICK_NONE;
    case PF_EVENT_POINTER_UP: {
        if (q->dragging) {
            set_slider(q, s, q->dragging, ev->x, 0);
            q->dragging = HIT_NONE;
            quick_draw(q, s);
            return QUICK_NONE;
        }
        /* Свайп вверх или касание под шторкой — закрыть. */
        if (q->down_y - ev->y > 100 || ev->y >= q->height)
            return QUICK_CLOSE;
        switch (hit(q, ev->x, ev->y)) {
        case HIT_LIGHT:
            pf_light_set(q->bright > 0 ? 0 : 30, q->warm);
            quick_draw(q, s);
            return QUICK_NONE;
        case HIT_WIFI:
            pf_wifi_set(!q->wifi);
            quick_draw(q, s);
            return QUICK_NONE;
        case HIT_CLEAN:
            return QUICK_CLEAN;
        default:
            return QUICK_NONE;
        }
    }
    default:
        return QUICK_NONE;
    }
}
