#include "widgets.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

int ui_inside(ui_rect r, int x, int y)
{
    return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static const gfx_style status_style = { FONT_MEDIUM, 32, UI_BLACK };

void ui_status(gfx *g, pf_screen *s)
{
    char buf[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(buf, sizeof buf, "%d:%02d", tm.tm_hour, tm.tm_min);
    gfx_text(g, s, UI_MARGIN, 62, status_style, buf);

    int charging = 0, pct = pf_battery(&charging);
    if (pct < 0)
        return;
    if (pct > 100)
        pct = 100;
    /* Батарейка: корпус 52×26, носик справа, заполнение по заряду. */
    int bw = 52, bh = 26, bx = s->width - UI_MARGIN - bw - 6, by = 38;
    gfx_rrect_line(g, s, bx, by, bw, bh, 5, 2.5f, UI_BLACK);
    gfx_rrect(g, s, bx + bw + 2, by + 8, 5, bh - 16, 1.5f, UI_BLACK);
    int fill = (bw - 10) * pct / 100;
    if (fill > 0)
        gfx_rrect(g, s, bx + 5, by + 5, fill, bh - 10, 2, UI_BLACK);
    snprintf(buf, sizeof buf, charging ? "зарядка · %d %%" : "%d %%", pct);
    int tw = gfx_text_width(g, status_style, buf);
    gfx_text(g, s, bx - 18 - tw, 62, status_style, buf);
}

static const char *const tab_labels[TAB_COUNT] = { "Книги", "Библиотека", "Приложения", "Настройки" };
static const gfx_icon_id tab_icons[TAB_COUNT] = { ICON_BOOK, ICON_LIBRARY, ICON_APPS, ICON_SETTINGS };

void ui_tabs(gfx *g, pf_screen *s, int active)
{
    int top = s->height - UI_TABS_H, w = s->width / TAB_COUNT;
    gfx_fill(s, 0, top, s->width, UI_TABS_H, UI_WHITE);
    gfx_fill(s, 0, top, s->width, 2, UI_LINE);
    for (int i = 0; i < TAB_COUNT; i++) {
        int on = i == active, cx = i * w + w / 2, size = 60;
        uint8_t gray = on ? UI_BLACK : UI_TEXT3;
        gfx_icon_ex(g, s, tab_icons[i], cx - size / 2, top + 26, size, 0, on, gray);
        gfx_style st = { on ? FONT_MEDIUM : FONT_REGULAR, 30, gray };
        int tw = gfx_text_width(g, st, tab_labels[i]);
        gfx_text(g, s, cx - tw / 2, top + 132, st, tab_labels[i]);
    }
}

int ui_tab_at(const pf_screen *s, int x, int y)
{
    if (y < s->height - UI_TABS_H || x < 0 || x >= s->width)
        return -1;
    return x * TAB_COUNT / s->width;
}

ui_header_hits ui_header(gfx *g, pf_screen *s, const char *title, int back, int page, int pages)
{
    ui_header_hits hits;
    memset(&hits, 0, sizeof hits);
    int top = UI_STATUS_H, base = top + 96, x = UI_MARGIN, right = s->width - UI_MARGIN;
    if (back) {
        gfx_icon(g, s, ICON_BACK, x - 14, base - 56, 64, 0, UI_BLACK);
        hits.back = (ui_rect){ 0, top, x + 90, UI_HEADER_H };
        x += 70;
    }
    if (pages > 1) {
        /* «‹ 2 / 5 ›»: стрелки — отдельные области касания. */
        char buf[32];
        snprintf(buf, sizeof buf, "%d / %d", page + 1, pages);
        gfx_style st = { FONT_REGULAR, 34, UI_TEXT2 };
        int tw = gfx_text_width(g, st, buf), arrow = 56;
        int nx = right - arrow, tx = nx - 16 - tw, px = tx - 16 - arrow;
        gfx_icon(g, s, ICON_BACK, px, base - 44, arrow, 0, page > 0 ? UI_BLACK : UI_LINE);
        gfx_text(g, s, tx, base - 6, st, buf);
        gfx_icon(g, s, ICON_NEXT, nx, base - 44, arrow, 0, page < pages - 1 ? UI_BLACK : UI_LINE);
        if (page > 0)
            hits.prev = (ui_rect){ px - 30, top, arrow + 60, UI_HEADER_H };
        if (page < pages - 1)
            hits.next = (ui_rect){ nx - 30, top, s->width - nx + 30, UI_HEADER_H };
        right = px - 30;
    }
    gfx_style st = { FONT_MEDIUM, 58, UI_BLACK };
    gfx_text_fit(g, s, x, base, right - x, st, title);
    return hits;
}

int ui_content_top(void)
{
    return UI_STATUS_H + UI_HEADER_H;
}

int ui_content_bottom(const pf_screen *s)
{
    return s->height - UI_TABS_H;
}

void ui_caption(gfx *g, pf_screen *s, int x, int y, const char *text)
{
    gfx_style st = { FONT_MEDIUM, 31, UI_TEXT3 };
    gfx_text(g, s, x, y, st, text);
}

void ui_progress(gfx *g, pf_screen *s, int x, int y, int w, int h, float frac)
{
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    gfx_rrect(g, s, x, y, w, h, h / 2.0f, UI_LINE);
    int fw = (int)(w * frac + 0.5f);
    if (fw > 0)
        gfx_rrect(g, s, x, y, fw < h ? h : fw, h, h / 2.0f, UI_BLACK);
}

const char *ui_plural(int n, const char *one, const char *few, const char *many)
{
    int m10 = n % 10, m100 = n % 100;
    if (m10 == 1 && m100 != 11)
        return one;
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14))
        return few;
    return many;
}

void ui_flash(gfx *g, pf_screen *s, ui_rect r)
{
    if (r.w <= 0 || r.h <= 0)
        return;
    int x = r.x + 12, y = r.y + 6, w = r.w - 24, h = r.h - 12;
    gfx_rrect_line(g, s, x, y, w, h, 18, 5, UI_BLACK);
    pf_update(x, y, w, h, PF_UPDATE_FAST_BW);
}

void ui_card(gfx *g, pf_screen *s, int x, int y, int w, int h)
{
    gfx_rrect(g, s, x, y, w, h, UI_CARD_R, UI_WHITE);
}

int ui_chip(gfx *g, pf_screen *s, int right, int cy, const char *text)
{
    gfx_style st = { FONT_MEDIUM, 32, UI_BLACK };
    int tw = gfx_text_width(g, st, text), h = 64, w = tw + 48;
    gfx_rrect(g, s, right - w, cy - h / 2, w, h, h / 2.0f, UI_FAINT);
    gfx_text(g, s, right - w + 24, cy + 12, st, text);
    return right - w;
}

void ui_pill(gfx *g, pf_screen *s, int x, int y, int w, int h, const char *label, int filled)
{
    if (filled)
        gfx_rrect(g, s, x, y, w, h, h / 2.0f, UI_BLACK);
    else
        gfx_rrect_line(g, s, x, y, w, h, h / 2.0f, 3, UI_BLACK);
    gfx_style st = { FONT_MEDIUM, 36, filled ? UI_WHITE : UI_BLACK };
    int tw = gfx_text_width(g, st, label);
    gfx_text(g, s, x + (w - tw) / 2, y + h / 2 + 13, st, label);
}

void ui_card_row(gfx *g, pf_screen *s, int x, int y, int w, int h, const char *title, const char *sub,
                 const char *value, int chevron, int line)
{
    int pad = 40, right = x + w - pad;
    if (chevron) {
        gfx_icon(g, s, ICON_NEXT, right - 40, y + h / 2 - 20, 40, 0, UI_TEXT3);
        right -= 56;
    }
    if (value && *value)
        right = ui_chip(g, s, right, y + h / 2, value) - 24;
    gfx_style ts = { FONT_REGULAR, 40, UI_BLACK }, ss = { FONT_REGULAR, 30, UI_TEXT2 };
    if (sub && *sub) {
        gfx_text_fit(g, s, x + pad, y + h / 2 - 6, right - x - pad, ts, title);
        gfx_text_fit(g, s, x + pad, y + h / 2 + 38, right - x - pad, ss, sub);
    } else {
        gfx_text_fit(g, s, x + pad, y + h / 2 + 14, right - x - pad, ts, title);
    }
    if (line)
        gfx_fill(s, x + pad, y + h - 2, w - pad, 2, UI_FAINT);
}

void ui_format_size(char *buf, size_t len, long long size)
{
    double mb = size / 1048576.0;
    if (size < 0)
        buf[0] = 0;
    else if (size < 1048576)
        snprintf(buf, len, "%lld КБ", (size + 1023) / 1024);
    else
        snprintf(buf, len, mb < 10 ? "%.1f МБ" : "%.0f МБ", mb);
    char *dot = strchr(buf, '.');
    if (dot)
        *dot = ',';
}

void ui_path_crumbs(const char *rel, size_t n, char *out, size_t len)
{
    size_t o = 0;
    for (const char *p = rel; p < rel + n && *p && o + 4 < len; p++) {
        if (*p == '/') {
            memcpy(out + o, " / ", 3);
            o += 3;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

int ui_slider_x(const pf_screen *s, int *w)
{
    *w = s->width - 2 * UI_MARGIN - 40;
    return UI_MARGIN + 20;
}

void ui_slider_track(gfx *g, pf_screen *s, int x, int cy, int w, float frac)
{
    frac = frac < 0 ? 0 : frac > 1 ? 1 : frac;
    int kx = x + (int)(w * frac);
    gfx_rrect(g, s, x, cy - 5, w, 10, 5, UI_LINE);
    gfx_rrect(g, s, x, cy - 5, kx - x + 10, 10, 5, UI_BLACK);
    gfx_rrect(g, s, kx - 20, cy - 20, 40, 40, 20, UI_BLACK);
}

int ui_slider_percent(const pf_screen *s, int x)
{
    int tw, tx = ui_slider_x(s, &tw), v = (x - tx) * 100 / tw;
    return v < 0 ? 0 : v > 100 ? 100 : v;
}

void ui_slider_row(gfx *g, pf_screen *s, int y, int h, const char *label, int percent)
{
    char v[16];
    snprintf(v, sizeof v, "%d %%", percent);
    gfx_style ls = { FONT_REGULAR, 36, UI_BLACK }, vs = { FONT_MEDIUM, 36, UI_BLACK };
    gfx_fill(s, 0, y, s->width, h, UI_WHITE);
    gfx_text(g, s, UI_MARGIN, y + 46, ls, label);
    gfx_text(g, s, s->width - UI_MARGIN - gfx_text_width(g, vs, v), y + 46, vs, v);
    int tw, tx = ui_slider_x(s, &tw);
    ui_slider_track(g, s, tx, y + 102, tw, percent / 100.0f);
}
