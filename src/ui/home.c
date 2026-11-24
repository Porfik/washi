#include "home.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "bookcover.h"
#include "widgets.h"

/* Как в Яндекс Книгах: жирные заголовки разделов, крупные обложки со скруглёнными углами,
 * прогресс под обложкой, чёрная кнопка-пилюля «Читать». */
#define CARD_Y 204
#define CARD_H 360
#define COVER_BOX_W 270
#define COVER_R 14
#define GRID_COLS 4
#define GRID_GAP 36
#define GRID_ITEM_TEXT_H 104 /* под обложкой: прогресс и название */
#define GRID_ROW_GAP 30
#define MAX_SLOTS 12

typedef struct {
    ui_rect r;     /* область касания */
    ui_rect cover; /* куда рисовать обложку */
    int radius;    /* скругление обложки */
    int waiting;   /* обложка ещё не готова */
    char path[1024];
    char sub[512]; /* подпись, с которой нарисована строка (автор появляется вместе с обложкой) */
} slot;

struct home {
    gfx *g;
    const positions *pos;
    covers *cv;
    char root[1024];
    slot slots[MAX_SLOTS];
    int nslots;
};

home *home_new(gfx *g, const positions *pos, covers *cv, const char *root)
{
    home *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->g = g;
    h->pos = pos;
    h->cv = cv;
    snprintf(h->root, sizeof h->root, "%s", root);
    return h;
}

void home_free(home *h)
{
    free(h);
}

/* Хвосты названий, которые убирать (config.txt: title_strip, через «|»). */
static char strip_list[512];

void home_set_title_strip(const char *list)
{
    snprintf(strip_list, sizeof strip_list, "%s", list ? list : "");
}

/* Убрать с конца out хвосты из strip_list (без учёта регистра) и пробелы перед ними. */
static size_t strip_tails(char *out, size_t n)
{
    for (int again = 1; again;) {
        again = 0;
        for (const char *t = strip_list; *t;) {
            const char *end = strchr(t, '|');
            size_t tl = end ? (size_t)(end - t) : strlen(t);
            const char *tok = t;
            while (tl && *tok == ' ')
                tok++, tl--;
            while (tl && tok[tl - 1] == ' ')
                tl--;
            if (tl && n > tl && !strncasecmp(out + n - tl, tok, tl)) {
                n -= tl;
                while (n && (out[n - 1] == ' ' || out[n - 1] == '_'))
                    n--;
                out[n] = 0;
                again = 1;
            }
            t = end ? end + 1 : t + strlen(t);
        }
    }
    return n;
}

void home_book_title(const char *path, char *out, size_t len)
{
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    snprintf(out, len, "%s", base);
    size_t n = strlen(out);
    /* Без расширения («.pdf», «.epub», «.djvu»…): точка в последних 6 знаках, после неё — буквы и цифры. */
    char *dot = strrchr(out, '.');
    if (dot && dot > out && n - (size_t)(dot - out) <= 6) {
        int ext = 1;
        for (const char *p = dot + 1; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')))
                ext = 0;
        if (ext && dot[1]) {
            *dot = 0;
            n = (size_t)(dot - out);
        }
    }
    if (*strip_list)
        strip_tails(out, n); /* хвост во всё название не убирается: что-то должно остаться */
    for (char *p = out; *p; p++)
        if (*p == '_')
            *p = ' ';
}

/* Папка книги относительно корня библиотеки: «Русский / Конструирование». */
static void folder_of(const home *h, const char *path, char *out, size_t len)
{
    size_t rl = strlen(h->root);
    const char *rel = !strncmp(path, h->root, rl) && path[rl] == '/' ? path + rl + 1 : path;
    const char *slash = strrchr(rel, '/');
    ui_path_crumbs(rel, slash ? (size_t)(slash - rel) : 0, out, len);
}

/* Обложка во всю рамку (bookcover.h). 1 — обложка ещё рисуется. */
static int draw_cover(home *h, pf_screen *s, ui_rect box, int radius, const char *path)
{
    return ui_book_cover(h->g, s, h->cv, box, radius, path, UI_WHITE);
}

static slot *add_slot(home *h, ui_rect r, ui_rect cover, int radius, const char *path)
{
    if (h->nslots >= MAX_SLOTS)
        return NULL;
    slot *sl = &h->slots[h->nslots++];
    sl->r = r;
    sl->cover = cover;
    sl->radius = radius;
    sl->waiting = 0;
    sl->sub[0] = 0;
    snprintf(sl->path, sizeof sl->path, "%s", path);
    return sl;
}

static void subtitle(home *h, const char *path, char *out, size_t len)
{
    const cover_info *ci = NULL;
    if (covers_get(h->cv, path, &ci) == 0 && ci->author[0])
        snprintf(out, len, "%s", ci->author);
    else
        folder_of(h, path, out, len);
}

static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Страниц в книге: из места чтения или из сведений PDF. */
static int pages_of(home *h, const position *p)
{
    const cover_info *ci = NULL;
    if (p->count > 0)
        return p->count;
    return covers_get(h->cv, p->path, &ci) == 0 ? ci->pages : 0;
}

/* Чёрная кнопка-пилюля с белой надписью. */
static void pill(home *h, pf_screen *s, int x, int y, int w, int hgt, const char *label)
{
    gfx_rrect(h->g, s, x, y, w, hgt, hgt / 2.0f, UI_BLACK);
    gfx_style st = { FONT_MEDIUM, 36, UI_WHITE };
    int tw = gfx_text_width(h->g, st, label);
    gfx_text(h->g, s, x + (w - tw) / 2, y + hgt / 2 + 13, st, label);
}

static void heading(home *h, pf_screen *s, int y, const char *text)
{
    gfx_style st = { FONT_BOLD, 46, UI_BLACK };
    gfx_text(h->g, s, UI_MARGIN, y, st, text);
}

static void draw_card(home *h, pf_screen *s, const position *p)
{
    gfx *g = h->g;
    int right = s->width - UI_MARGIN;
    ui_rect cover = { UI_MARGIN, CARD_Y, COVER_BOX_W, CARD_H };
    slot *sl = add_slot(h, (ui_rect){ 0, CARD_Y - 20, s->width, CARD_H + 40 }, cover, COVER_R, p->path);
    if (draw_cover(h, s, cover, COVER_R, p->path) == 1 && sl)
        sl->waiting = 1;

    int x = UI_MARGIN + COVER_BOX_W + 48, w = right - x;
    int by = CARD_Y + CARD_H, bar_y = by - 160;
    /* Название — столько строк, сколько влезает над папкой так, чтобы папка не заходила на
     * полосу прогресса (её низ букв — на 22 ниже строки); остальное — «…». */
    enum { TITLE_Y = CARD_Y + 40, TITLE_LH = 58, SUB_GAP = 54, SUB_DESC = 22 };
    int max_lines = 1 + (bar_y - SUB_DESC - SUB_GAP - TITLE_Y) / TITLE_LH;
    max_lines = max_lines < 1 ? 1 : max_lines > 3 ? 3 : max_lines;
    char title[512], sub[512];
    home_book_title(p->path, title, sizeof title);
    gfx_style ts = { FONT_BOLD, 46, UI_BLACK };
    int lines = gfx_text_wrap(g, s, x, TITLE_Y, w, TITLE_LH, max_lines, ts, title);
    subtitle(h, p->path, sub, sizeof sub);
    if (sl)
        snprintf(sl->sub, sizeof sl->sub, "%s", sub);
    gfx_style as = { FONT_REGULAR, 34, UI_TEXT2 };
    gfx_text_fit(g, s, x, TITLE_Y + (lines - 1) * TITLE_LH + SUB_GAP, w, as, sub);

    /* Низ: прогресс и «Читать». */
    int count = pages_of(h, p);
    char where[64];
    if (count > 0)
        snprintf(where, sizeof where, "стр. %d из %d", p->page + 1, count);
    else
        snprintf(where, sizeof where, "стр. %d", p->page + 1);
    gfx_style ws = { FONT_REGULAR, 31, UI_TEXT2 };
    ui_progress(g, s, x, bar_y, w, 10, count > 0 ? (float)(p->page + 1) / count : 0);
    gfx_text(g, s, x, by - 112, ws, where);
    pill(h, s, x, by - 84, 300, 84, "Читать");
}

static void draw_empty(home *h, pf_screen *s)
{
    gfx_style bs = { FONT_REGULAR, 38, UI_TEXT2 };
    gfx_text_wrap(h->g, s, UI_MARGIN, CARD_Y + 60, s->width - 2 * UI_MARGIN, 52, 3, bs,
                  "Откройте книгу во вкладке «Библиотека»");
}

void home_draw(home *h, pf_screen *s)
{
    int bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_WHITE);
    h->nslots = 0;

    /* Первая существующая книга из недавних — на карточку, следующие — сеткой. */
    int i = 0;
    while (i < h->pos->count && !file_exists(h->pos->items[i].path))
        i++;
    heading(h, s, CARD_Y - 34, "Читаю сейчас");
    if (i >= h->pos->count) {
        draw_empty(h, s);
        return;
    }
    draw_card(h, s, &h->pos->items[i]);

    /* «Недавние» — только если есть что показать: пустой заголовок выглядит как поломка. */
    int more = 0;
    for (int j = i + 1; j < h->pos->count && !more; j++)
        more = file_exists(h->pos->items[j].path);
    if (!more)
        return;
    int top = CARD_Y + CARD_H + 92;
    heading(h, s, top, "Недавние");

    int cw = (s->width - 2 * UI_MARGIN - (GRID_COLS - 1) * GRID_GAP) / GRID_COLS;
    int gy = top + 34, avail = bottom - gy - 10;
    int item_h = (avail - GRID_ROW_GAP) / 2, ch = item_h - GRID_ITEM_TEXT_H;
    /* Все обложки — одной рамкой 3 : 4 (по ширине колонки, если влезает по высоте). */
    int bw = cw;
    if (ch > BOOKCOVER_H(cw))
        ch = BOOKCOVER_H(cw);
    else
        bw = ch * 3 / 4;
    int shown = 0;
    for (i++; i < h->pos->count && shown < 2 * GRID_COLS; i++) {
        const position *p = &h->pos->items[i];
        if (!file_exists(p->path))
            continue;
        int col = shown % GRID_COLS, row = shown / GRID_COLS;
        int x = UI_MARGIN + col * (cw + GRID_GAP), y = gy + row * (item_h + GRID_ROW_GAP);
        shown++;
        ui_rect cover = { x + (cw - bw) / 2, y, bw, ch };
        slot *sl = add_slot(h, (ui_rect){ x - GRID_GAP / 2, y, cw + GRID_GAP, item_h }, cover, 10, p->path);
        if (draw_cover(h, s, cover, 10, p->path) == 1 && sl)
            sl->waiting = 1;
        if (sl)
            subtitle(h, p->path, sl->sub, sizeof sl->sub);

        /* Под обложкой — название и прогресс (как в библиотеке). */
        int tx = cover.x;
        char title[512];
        home_book_title(p->path, title, sizeof title);
        gfx_style ts = { FONT_REGULAR, 29, UI_BLACK };
        gfx_text_fit(h->g, s, tx, y + ch + 42, bw, ts, title);
        int count = pages_of(h, p);
        char pct[16] = "";
        if (count > 0)
            snprintf(pct, sizeof pct, "%d %%", (p->page + 1) * 100 / count);
        gfx_style ps = { FONT_REGULAR, 26, UI_TEXT2 };
        int pw = gfx_text_width(h->g, ps, pct);
        ui_progress(h->g, s, tx, y + ch + 68, bw - pw - 14, 6, count > 0 ? (float)(p->page + 1) / count : 0);
        gfx_text(h->g, s, tx + bw - pw, y + ch + 80, ps, pct);
    }
}

int home_covers_ready(home *h, pf_screen *s, int *text_changed)
{
    int waiting = 0;
    *text_changed = 0;
    for (int i = 0; i < h->nslots; i++) {
        slot *sl = &h->slots[i];
        if (!sl->waiting)
            continue;
        const cover_info *ci = NULL;
        int rc = covers_get(h->cv, sl->path, &ci);
        if (rc == 1) {
            waiting++;
            continue;
        }
        sl->waiting = 0;
        if (rc == 0) {
            char sub[512];
            subtitle(h, sl->path, sub, sizeof sub);
            if (strcmp(sub, sl->sub))
                *text_changed = 1;
            draw_cover(h, s, sl->cover, sl->radius, sl->path);
            pf_update(sl->cover.x, sl->cover.y, sl->cover.w, sl->cover.h, PF_UPDATE_PARTIAL);
        }
    }
    return waiting;
}

int home_tap(home *h, int x, int y, char *path, size_t len, ui_rect *hit)
{
    for (int i = 0; i < h->nslots; i++)
        if (ui_inside(h->slots[i].r, x, y)) {
            snprintf(path, len, "%s", h->slots[i].path);
            *hit = h->slots[i].r;
            return 1;
        }
    return 0;
}
