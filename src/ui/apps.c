#define _DEFAULT_SOURCE
#include "apps.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../base/config.h"
#include "../base/log.h"
#include "widgets.h"

#define MAX_APPS 128
#define COLS 3
#define TILE_GAP 30
#define TILE_H 300
#define ICON_SIZE 104

typedef struct {
    char name[128];
    char path[1024];
    int icon;     /* gfx_icon_id */
    char *svg;    /* свой значок дополнения (icon.svg): контур, или NULL */
    float vb[4];  /* его viewBox */
    float stroke; /* > 0 — линии такой толщины (в единицах viewBox), 0 — заливка */
} app_item;

struct apps {
    gfx *g;
    app_item items[MAX_APPS];
    int n;
    int page, per_page, pages;
    int selected;
    ui_header_hits hits;
    ui_rect tiles[MAX_APPS]; /* плитки текущей страницы */
    int ntiles, first;
};

/* Встроенные приложения прошивки, которые есть смысл запускать из хаба. */
static const struct {
    const char *name, *file;
    gfx_icon_id icon;
} builtin[] = {
    { "Настройки прошивки", "settings.app", ICON_SETTINGS },
    { "Словарь", "Dictionary.app", ICON_DICTIONARY },
    { "Заметки", "notes.app", ICON_NOTES },
    { "Рисование", "scribble.app", ICON_DRAW },
    { "Календарь", "calendar.app", ICON_CALENDAR },
    { "Калькулятор", "calc.app", ICON_CALC },
    { "Фото", "picviewer.app", ICON_IMAGE },
    { "Браузер", "browser.app", ICON_GLOBE },
    { "Шахматы", "chess.app", ICON_CHESS },
    { "Судоку", "sudoku.app", ICON_SUDOKU },
    { "Пасьянс", "kosynka.app", ICON_CARDS },
};

static app_item *add(apps *a, const char *name, const char *path, int icon)
{
    if (a->n >= MAX_APPS)
        return NULL;
    app_item *it = &a->items[a->n++];
    memset(it, 0, sizeof *it);
    snprintf(it->name, sizeof it->name, "%s", name);
    snprintf(it->path, sizeof it->path, "%s", path);
    it->icon = icon;
    return it;
}

/* Значок дополнения из icon.svg: viewBox и все path d (других фигур нет — только контуры).
 * Линии — если у <svg> fill="none"; толщина — stroke-width (по умолчанию 2). */
static void load_svg_icon(app_item *it, const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    char *buf = malloc(65536);
    size_t n = buf ? fread(buf, 1, 65535, f) : 0;
    fclose(f);
    if (!buf)
        return;
    buf[n] = 0;
    const char *vb = strstr(buf, "viewBox=\"");
    float v[4] = { 0, 0, 24, 24 };
    if (vb)
        sscanf(vb + 9, "%f%*[ ,]%f%*[ ,]%f%*[ ,]%f", &v[0], &v[1], &v[2], &v[3]);
    const char *svg = strstr(buf, "<svg");
    const char *svg_end = svg ? strchr(svg, '>') : NULL;
    int stroke = 0;
    float sw = 2;
    if (svg && svg_end) {
        const char *fn = strstr(svg, "fill=\"none\"");
        stroke = fn && fn < svg_end;
        const char *w = strstr(svg, "stroke-width=\"");
        if (w && w < svg_end)
            sw = strtof(w + 14, NULL);
    }
    size_t cap = n + 1024, len = 0;
    char *d = malloc(cap);
    /* Элементы по порядку: path d, а circle, rect, line — переводятся в контуры. */
    for (const char *p = buf; d && (p = strchr(p, '<')) != NULL; p++) {
        char tmp[512];
        tmp[0] = 0;
        if (!strncmp(p, "<path", 5)) {
            const char *q = strstr(p, " d=\"");
            const char *gt = strchr(p, '>');
            if (!q || (gt && q > gt))
                continue;
            q += 4;
            const char *e = strchr(q, '"');
            if (!e)
                break;
            size_t k = (size_t)(e - q);
            if (len + k + 16 >= cap)
                break;
            if (*q == 'm') /* первый «m» элемента — от начала координат (см. scripts/icons.py) */
                len += (size_t)snprintf(d + len, cap - len, "M0 0 ");
            memcpy(d + len, q, k);
            len += k;
            d[len++] = ' ';
            continue;
        }
        float cx = 0, cy = 0, r = 0, x = 0, y = 0, w = 0, h = 0, x1 = 0, y1 = 0, x2 = 0, y2 = 0;
        const char *gt = strchr(p, '>');
        size_t elen = gt ? (size_t)(gt - p) : strlen(p);
        char el[400];
        snprintf(el, sizeof el, "%.*s", (int)(elen < sizeof el - 1 ? elen : sizeof el - 1), p);
#define ATTR(name, var) do { const char *a_ = strstr(el, " " name "=\""); if (a_) var = strtof(a_ + strlen(name) + 3, NULL); } while (0)
        if (!strncmp(el, "<circle", 7)) {
            ATTR("cx", cx);
            ATTR("cy", cy);
            ATTR("r", r);
            snprintf(tmp, sizeof tmp, "M%g %ga%g %g 0 1 0 %g 0a%g %g 0 1 0 %g 0Z ", cx - r, cy, r, r, 2 * r, r, r, -2 * r);
        } else if (!strncmp(el, "<rect", 5)) {
            ATTR("x", x);
            ATTR("y", y);
            ATTR("width", w);
            ATTR("height", h);
            snprintf(tmp, sizeof tmp, "M%g %gh%gv%gh%gZ ", x, y, w, h, -w);
        } else if (!strncmp(el, "<line", 5)) {
            ATTR("x1", x1);
            ATTR("y1", y1);
            ATTR("x2", x2);
            ATTR("y2", y2);
            snprintf(tmp, sizeof tmp, "M%g %gL%g %g ", x1, y1, x2, y2);
        }
#undef ATTR
        size_t k = strlen(tmp);
        if (k && len + k < cap) {
            memcpy(d + len, tmp, k);
            len += k;
        }
    }
    free(buf);
    if (!d || len == 0) {
        free(d);
        return;
    }
    d[len] = 0;
    it->svg = d;
    memcpy(it->vb, v, sizeof v);
    it->stroke = stroke ? (sw > 0 ? sw : 2) : 0;
}

static void clear_items(apps *a)
{
    for (int i = 0; i < a->n; i++)
        free(a->items[i].svg);
    a->n = 0;
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int name_cmp(const void *x, const void *y)
{
    return strcasecmp(((const app_item *)x)->name, ((const app_item *)y)->name);
}

/* Дополнения хаба: <папка данных>/apps/ * /app.txt. */
static void scan_extensions(apps *a)
{
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/apps", pf_data_dir());
    DIR *dp = opendir(dir);
    if (!dp)
        return;
    int first = a->n;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.')
            continue;
        char manifest[1300], exec[1300];
        snprintf(manifest, sizeof manifest, "%s/%s/app.txt", dir, e->d_name);
        config c;
        if (config_load(&c, manifest) != 0)
            continue;
        const char *x = config_get(&c, "exec", NULL);
        if (!x)
            continue;
        if (x[0] == '/')
            snprintf(exec, sizeof exec, "%s", x);
        else
            snprintf(exec, sizeof exec, "%s/%s/%s", dir, e->d_name, x);
        if (!exists(exec)) {
            log_msg("приложения: %s — нет %s", manifest, exec);
            continue;
        }
        app_item *it = add(a, config_get(&c, "name", e->d_name), exec, ICON_PUZZLE);
        char icon[1300];
        snprintf(icon, sizeof icon, "%s/%s/icon.svg", dir, e->d_name);
        if (it)
            load_svg_icon(it, icon);
    }
    closedir(dp);
    qsort(a->items + first, (size_t)(a->n - first), sizeof *a->items, name_cmp);
}

/* Установленные: .app в /mnt/ext1/applications, кроме самого хаба. */
static void scan_installed(apps *a)
{
    const char *dir = "/mnt/ext1/applications";
    DIR *dp = opendir(dir);
    if (!dp)
        return;
    int first = a->n;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        size_t n = strlen(e->d_name);
        if (e->d_name[0] == '.' || n < 5 || strcasecmp(e->d_name + n - 4, ".app") ||
            !strncasecmp(e->d_name, "washi", 5))
            continue;
        char path[1300], name[128];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        snprintf(name, sizeof name, "%.*s", (int)(n - 4), e->d_name);
        if (name[0] == '@')
            memmove(name, name + 1, strlen(name));
        add(a, name, path, !strncasecmp(name, "koreader", 8) ? ICON_BOOK : ICON_APP);
    }
    closedir(dp);
    qsort(a->items + first, (size_t)(a->n - first), sizeof *a->items, name_cmp);
}

void apps_refresh(apps *a)
{
    clear_items(a);
    /* Свои экраны хаба (page.h) — первыми: путь «hub:…» хаб открывает сам. */
    add(a, "Приём по Wi-Fi", APPS_HUB_RECV, ICON_DOWNLOAD);
    add(a, "Каталоги", APPS_HUB_OPDS, ICON_CATALOG);
    add(a, "Синхронизация", APPS_HUB_SYNC, ICON_SYNC);
    scan_extensions(a);
    scan_installed(a);
    /* На ПК для проверки можно подложить папку с «прошивкой»: WASHI_SYSTEM_BIN. */
    const char *sysbin = getenv("WASHI_SYSTEM_BIN");
    for (size_t i = 0; i < sizeof builtin / sizeof *builtin; i++) {
        char path[512];
        snprintf(path, sizeof path, "%s/%s", sysbin ? sysbin : "/ebrmain/bin", builtin[i].file);
        if (exists(path))
            add(a, builtin[i].name, path, builtin[i].icon);
    }
}

apps *apps_new(gfx *g)
{
    apps *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->g = g;
    a->selected = -1;
    apps_refresh(a);
    return a;
}

void apps_free(apps *a)
{
    if (a)
        clear_items(a);
    free(a);
}

/* Плитки 3 в ряд, как на экране телефона: белая карточка, значок, название. */
void apps_draw(apps *a, pf_screen *s)
{
    gfx *g = a->g;
    int top = ui_content_top(), bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_BG);
    int tw = (s->width - 2 * UI_MARGIN - (COLS - 1) * TILE_GAP) / COLS;
    int rows = (bottom - top - 10) / (TILE_H + TILE_GAP);
    if (rows < 1)
        rows = 1;
    a->per_page = rows * COLS;
    a->pages = a->n ? (a->n + a->per_page - 1) / a->per_page : 1;
    if (a->page >= a->pages)
        a->page = a->pages - 1;
    a->hits = ui_header(g, s, "Приложения", 0, a->page, a->pages);
    a->first = a->page * a->per_page;
    a->ntiles = 0;
    for (int i = 0; i < a->per_page && a->first + i < a->n; i++) {
        const app_item *it = &a->items[a->first + i];
        int x = UI_MARGIN + (i % COLS) * (tw + TILE_GAP), y = top + (i / COLS) * (TILE_H + TILE_GAP);
        ui_card(g, s, x, y, tw, TILE_H);
        int ix = x + (tw - ICON_SIZE) / 2, iy = y + 58;
        if (it->svg)
            gfx_svg(g, s, it->svg, it->vb, ix, iy, ICON_SIZE,
                    it->stroke > 0 ? it->stroke * ICON_SIZE / (it->vb[2] > it->vb[3] ? it->vb[2] : it->vb[3]) : 0,
                    UI_BLACK);
        else
            gfx_icon(g, s, (gfx_icon_id)it->icon, ix, iy, ICON_SIZE, 0, UI_BLACK);
        gfx_style ns = { FONT_MEDIUM, 32, UI_BLACK };
        int nw = gfx_text_width(g, ns, it->name);
        if (nw > tw - 40)
            nw = tw - 40;
        gfx_text_fit(g, s, x + (tw - nw) / 2, y + 236, tw - 40, ns, it->name);
        a->tiles[a->ntiles++] = (ui_rect){ x, y, tw, TILE_H };
    }
    if (a->n == 0) {
        gfx_style st = { FONT_REGULAR, 40, UI_TEXT2 };
        gfx_text_wrap(g, s, UI_MARGIN, top + 80, s->width - 2 * UI_MARGIN, 52, 3, st, "Приложений не найдено.");
    }
}

static int turn(apps *a, int d)
{
    int p = a->page + d;
    if (p < 0 || p >= a->pages)
        return 0;
    a->page = p;
    return 1;
}

apps_action apps_key(apps *a, pf_key key)
{
    if (key == PF_KEY_NEXT)
        return turn(a, 1) ? APPS_REDRAW : APPS_NONE;
    if (key == PF_KEY_PREV)
        return turn(a, -1) ? APPS_REDRAW : APPS_NONE;
    return APPS_NONE;
}

apps_action apps_tap(apps *a, int x, int y)
{
    if (ui_inside(a->hits.prev, x, y))
        return turn(a, -1) ? APPS_REDRAW : APPS_NONE;
    if (ui_inside(a->hits.next, x, y))
        return turn(a, 1) ? APPS_REDRAW : APPS_NONE;
    for (int i = 0; i < a->ntiles; i++)
        if (ui_inside(a->tiles[i], x, y)) {
            a->selected = a->first + i;
            return APPS_LAUNCH;
        }
    return APPS_NONE;
}

const char *apps_selected(const apps *a)
{
    return a->selected >= 0 ? a->items[a->selected].path : "";
}

const char *apps_selected_name(const apps *a)
{
    return a->selected >= 0 ? a->items[a->selected].name : "";
}

ui_rect apps_hit(const apps *a)
{
    int i = a->selected - a->first;
    return i >= 0 && i < a->ntiles ? a->tiles[i] : (ui_rect){ 0, 0, 0, 0 };
}
