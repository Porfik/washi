#include "filedlg.h"

#include <stdio.h>
#include <string.h>

#define BTN_H 108
#define GAP 20
#define PAD 48

void filedlg_show(filedlg *d, int kind, const char *title, const char *text, int n, const char *const *label,
                  const int *filled)
{
    memset(d, 0, sizeof *d);
    d->open = 1;
    d->kind = kind;
    snprintf(d->title, sizeof d->title, "%s", title);
    snprintf(d->text, sizeof d->text, "%s", text);
    d->n = n > FILEDLG_MAX_BTN ? FILEDLG_MAX_BTN : n;
    for (int i = 0; i < d->n; i++) {
        snprintf(d->label[i], sizeof d->label[i], "%s", label[i]);
        d->filled[i] = filled[i];
    }
}

void filedlg_draw(filedlg *d, gfx *g, pf_screen *s)
{
    int top = ui_content_top(), bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_WHITE);
    int x = 96, w = s->width - 192, tw = w - 2 * PAD;
    gfx_style ts = { FONT_MEDIUM, 46, UI_BLACK }, bs = { FONT_REGULAR, 36, UI_TEXT2 };
    int stacked = d->n > 2;
    int rows = stacked ? d->n : 1;
    int tl = gfx_text_wrap(g, NULL, 0, 0, tw, 58, 2, ts, d->title);
    int xl = *d->text ? gfx_text_wrap(g, NULL, 0, 0, tw, 50, 4, bs, d->text) : 0;
    int h = PAD + tl * 58 + (xl ? 20 + xl * 50 : 0) + 40 + rows * BTN_H + (rows - 1) * GAP + PAD;
    int y = top + (bottom - top - h) / 2 - 40;
    ui_card(g, s, x, y, w, h);
    gfx_rrect_line(g, s, x, y, w, h, UI_CARD_R, 4, UI_BLACK);
    gfx_text_wrap(g, s, x + PAD, y + PAD + 44, tw, 58, 2, ts, d->title);
    if (xl)
        gfx_text_wrap(g, s, x + PAD, y + PAD + tl * 58 + 20 + 38, tw, 50, 4, bs, d->text);
    int by = y + h - PAD - rows * BTN_H - (rows - 1) * GAP;
    if (stacked) {
        for (int i = 0; i < d->n; i++) {
            d->hit[i] = (ui_rect){ x + PAD, by + i * (BTN_H + GAP), tw, BTN_H };
            ui_pill(g, s, d->hit[i].x, d->hit[i].y, tw, BTN_H, d->label[i], d->filled[i]);
        }
    } else {
        int bw = (tw - GAP * (d->n - 1)) / (d->n ? d->n : 1);
        for (int i = 0; i < d->n; i++) {
            d->hit[i] = (ui_rect){ x + PAD + i * (bw + GAP), by, bw, BTN_H };
            ui_pill(g, s, d->hit[i].x, d->hit[i].y, bw, BTN_H, d->label[i], d->filled[i]);
        }
    }
}

int filedlg_tap(const filedlg *d, int x, int y)
{
    for (int i = 0; i < d->n; i++)
        if (ui_inside(d->hit[i], x, y))
            return i;
    return -1;
}
