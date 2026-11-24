#include "bookinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "bookcover.h"
#include "home.h"

#define INFO_COVER_W 400
#define INFO_COVER_H 533
#define BTN_H 100
#define BTN_GAP 16

struct bookinfo {
    gfx *g;
    covers *cv;
    const positions *pos;
    char root[1024];
    char path[1024];
    ui_header_hits hits;
    ui_rect cover, read, start, remove, rename, del;
    int waiting;
};

bookinfo *bookinfo_new(gfx *g, covers *cv, const positions *pos, const char *root)
{
    bookinfo *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->g = g;
    b->cv = cv;
    b->pos = pos;
    snprintf(b->root, sizeof b->root, "%s", root);
    return b;
}

void bookinfo_free(bookinfo *b)
{
    free(b);
}

void bookinfo_open(bookinfo *b, const char *path)
{
    snprintf(b->path, sizeof b->path, "%s", path);
}

const char *bookinfo_path(const bookinfo *b)
{
    return b->path;
}

/* Обложка во всю рамку (bookcover.h). 1 — ещё ждём. */
static int draw_cover(bookinfo *b, pf_screen *s)
{
    return ui_book_cover(b->g, s, b->cv, b->cover, 18, b->path, UI_WHITE);
}

/* Цифра с подписью под ней, по центру колонки. */
static void stat_cell(bookinfo *b, pf_screen *s, int cx, int y, const char *value, const char *label)
{
    gfx_style vs = { FONT_MEDIUM, 40, UI_BLACK }, ls = { FONT_REGULAR, 28, UI_TEXT3 };
    gfx_text(b->g, s, cx - gfx_text_width(b->g, vs, value) / 2, y, vs, value);
    gfx_text(b->g, s, cx - gfx_text_width(b->g, ls, label) / 2, y + 44, ls, label);
}

void bookinfo_draw(bookinfo *b, pf_screen *s)
{
    gfx *g = b->g;
    int W = s->width, M = UI_MARGIN, bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, W, bottom - UI_STATUS_H, UI_WHITE);
    b->hits = ui_header(g, s, "О книге", 1, 0, 1);
    int y = ui_content_top() - 30;

    b->cover = (ui_rect){ (W - INFO_COVER_W) / 2, y, INFO_COVER_W, INFO_COVER_H };
    b->waiting = draw_cover(b, s);
    y += INFO_COVER_H + 64;

    char title[512];
    home_book_title(b->path, title, sizeof title);
    gfx_style ts = { FONT_BOLD, 48, UI_BLACK };
    int lines = gfx_text_wrap_center(g, s, M, y, W - 2 * M, 60, 2, ts, title);
    y += lines * 60;
    const cover_info *ci = NULL;
    int have = covers_get(b->cv, b->path, &ci) == 0;
    if (have && ci->author[0]) {
        gfx_style as = { FONT_REGULAR, 36, UI_TEXT2 };
        gfx_text_wrap_center(g, s, M, y, W - 2 * M, 46, 1, as, ci->author);
        y += 46;
    }

    /* Цифры. */
    const position *p = positions_find(b->pos, b->path);
    int pages = p && p->count > 0 ? p->count : have ? ci->pages : 0;
    struct stat st;
    char v[3][48];
    snprintf(v[0], sizeof v[0], "%d", pages);
    if (p && pages > 0)
        snprintf(v[1], sizeof v[1], "%d %%", (p->page + 1) * 100 / pages);
    else
        snprintf(v[1], sizeof v[1], "—");
    if (stat(b->path, &st) == 0)
        ui_format_size(v[2], sizeof v[2], (long long)st.st_size);
    else
        snprintf(v[2], sizeof v[2], "—");
    static const char *const labels[3] = { "страниц", "прочитано", "размер" };
    int cw = (W - 2 * M) / 3;
    y += 30;
    for (int i = 0; i < 3; i++)
        stat_cell(b, s, M + i * cw + cw / 2, y + 40, v[i], labels[i]);
    y += 120;

    /* Действия. */
    b->read = (ui_rect){ M, y, W - 2 * M, BTN_H };
    ui_pill(g, s, M, y, W - 2 * M, BTN_H, p ? "Продолжить чтение" : "Читать", 1);
    y += BTN_H + BTN_GAP;
    memset(&b->start, 0, sizeof b->start);
    memset(&b->remove, 0, sizeof b->remove);
    if (p) {
        b->start = (ui_rect){ M, y, W - 2 * M, BTN_H };
        ui_pill(g, s, M, y, W - 2 * M, BTN_H, "Читать с начала", 0);
        y += BTN_H + BTN_GAP;
    }
    int hw = (W - 2 * M - BTN_GAP) / 2;
    b->rename = (ui_rect){ M, y, hw, BTN_H };
    b->del = (ui_rect){ M + hw + BTN_GAP, y, hw, BTN_H };
    ui_pill(g, s, b->rename.x, y, hw, BTN_H, "Переименовать", 0);
    ui_pill(g, s, b->del.x, y, hw, BTN_H, "Удалить", 0);
    y += BTN_H;
    if (p) {
        gfx_style rs = { FONT_REGULAR, 34, UI_TEXT2 };
        const char *rm = "Убрать из недавних";
        int rw = gfx_text_width(g, rs, rm);
        gfx_text(g, s, (W - rw) / 2, y + 62, rs, rm);
        b->remove = (ui_rect){ (W - rw) / 2 - 40, y + 8, rw + 80, 80 };
    }

    /* Папка книги — мелко внизу. */
    size_t rl = strlen(b->root);
    const char *rel = !strncmp(b->path, b->root, rl) && b->path[rl] == '/' ? b->path + rl + 1 : b->path;
    gfx_style fs = { FONT_REGULAR, 28, UI_TEXT3 };
    gfx_text_wrap_center(g, s, M, bottom - 34, W - 2 * M, 34, 1, fs, rel);
}

int bookinfo_cover_ready(bookinfo *b, pf_screen *s)
{
    if (!b->waiting)
        return 0;
    b->waiting = draw_cover(b, s);
    if (!b->waiting)
        pf_update(b->cover.x, b->cover.y, b->cover.w, b->cover.h, PF_UPDATE_PARTIAL);
    return b->waiting;
}

bookinfo_action bookinfo_tap(bookinfo *b, int x, int y, ui_rect *hit)
{
    *hit = (ui_rect){ 0, 0, 0, 0 };
    if (ui_inside(b->hits.back, x, y))
        return BOOKINFO_BACK;
    if (ui_inside(b->read, x, y) || ui_inside(b->cover, x, y)) {
        *hit = ui_inside(b->read, x, y) ? b->read : b->cover;
        return BOOKINFO_READ;
    }
    if (ui_inside(b->start, x, y)) {
        *hit = b->start;
        return BOOKINFO_START;
    }
    if (ui_inside(b->rename, x, y)) {
        *hit = b->rename;
        return BOOKINFO_RENAME;
    }
    if (ui_inside(b->del, x, y)) {
        *hit = b->del;
        return BOOKINFO_DELETE;
    }
    if (ui_inside(b->remove, x, y))
        return BOOKINFO_REMOVE;
    return BOOKINFO_NONE;
}
