/* _DEFAULT_SOURCE: d_type и DT_* в dirent.h. */
#define _DEFAULT_SOURCE
#include "library.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../base/clock.h"
#include "../base/config.h"
#include "../base/log.h"
#include "bookcover.h"
#include "home.h"
#include "widgets.h"

#define MAX_DEPTH 12

typedef struct lib_dir lib_dir;

enum { ST_NEW, ST_READING, ST_DONE };

typedef struct {
    char *name;  /* имя файла без .pdf */
    char *title; /* название для показа (home_book_title) */
    char *path;
    long long size;
    long long mtime;
    lib_dir *dir;
    int rank;   /* место в недавних (0 — открытая последней), INT_MAX — не открывали */
    int status; /* ST_* */
    int pct;    /* прочитано, %; -1 — неизвестно */
    int page;   /* где остановились, с 0 */
    int fmt;    /* формат: индекс в formats (0 — PDF) */
} lib_file;

struct lib_dir {
    char *path;
    const char *name; /* последняя часть path */
    lib_dir *parent;
    lib_dir **dirs;
    int ndirs;
    lib_file *files;
    int nfiles;
    int total; /* книг здесь и во вложенных папках */
    int shown; /* из них подходит под фильтр */
};

typedef enum { ITEM_DIR, ITEM_FILE } item_kind;

typedef struct {
    item_kind kind;
    lib_dir *dir;   /* ITEM_DIR */
    lib_file *file; /* ITEM_FILE */
} item;

/* Вид, сортировка, фильтр. */
enum { VIEW_GRID, VIEW_LIST, VIEW_FOLDERS, VIEW_COUNT };
enum { SORT_RECENT, SORT_TITLE, SORT_ADDED, SORT_COUNT };
enum { FILTER_ALL, FILTER_READING, FILTER_NEW, FILTER_DONE, FILTER_COUNT };

static const char *const view_keys[VIEW_COUNT] = { "covers", "list", "folders" };
static const char *const view_names[VIEW_COUNT] = { "Обложки", "Список", "Папки" };
static const char *const sort_keys[SORT_COUNT] = { "recent", "title", "added" };
static const char *const sort_names[SORT_COUNT] = { "Недавние", "Название", "Добавлены" };
static const char *const filter_names[FILTER_COUNT] = { "Все", "Читаю", "Новые", "Прочитано" };

#define FILTER_ROW_H 76  /* строка фильтров под заголовком */
#define GRID_COLS 4
#define GRID_ROWS 3
#define GRID_TEXT_H 96   /* под обложкой: название и прогресс */
#define GRID_ROW_GAP 24
#define LIST_THUMB_W 84  /* обложка в строке списка (3 : 4) */
#define SHEET_H 520      /* лист «Показывать» */
#define MAX_CELLS 32     /* книг и папок на одной странице */
#define POS_HASH 2048    /* таблица недавних: путь → место (больше POSITIONS_MAX вдвое) */

typedef struct {
    ui_rect hit;   /* область касания */
    ui_rect cover; /* обложка (.w == 0 — нет) */
    int radius;
    int waiting; /* обложка ещё рисуется */
    int item;    /* индекс в items */
} cell;

struct library {
    char root[1024];
    char config_path[1024];
    gfx *g;
    const positions *pos;
    covers *cv;
    lib_dir *tree;
    lib_dir *cur;
    item *items;
    int nitems, cap;
    lib_file **flat; /* все книги */
    int nflat;
    lib_file **tmp; /* книги папки при сборке списка */
    int view, sort, filter;
    int searching;
    char query[256];
    int sheet; /* открыт лист «Показывать» */
    int page;
    int per_page;
    int pages;
    cell cells[MAX_CELLS];
    int ncells;
    ui_rect filter_hits[FILTER_COUNT], sort_hit, view_hit, search_hit;
    ui_rect sheet_view[VIEW_COUNT], sheet_sort[SORT_COUNT];
    char current[1024]; /* открытая книга, "" — нет */
    char selected[1024];
    ui_rect selected_hit;
    char folder[1024]; /* LIBRARY_FOLDER */
    char folder_name[256];
    int folder_books;
    char keep_dir[1024]; /* открытая папка до library_reload */
    ui_header_hits hits;
    short pos_hash[POS_HASH];
};

/* ---- Поиск книг ---- */

/* Форматы книг: PDF открывает наша читалка, остальные — штатная (doc_supported). */
static const struct {
    const char *ext, *label;
} formats[] = {
    { ".pdf", "PDF" },   { ".epub", "EPUB" }, { ".djvu", "DjVu" }, { ".djv", "DjVu" }, { ".fb2", "FB2" },
    { ".fb2.zip", "FB2" },
    { ".mobi", "MOBI" }, { ".azw3", "AZW3" }, { ".cbz", "CBZ" },   { ".cbr", "CBR" },  { ".chm", "CHM" },
    { ".rtf", "RTF" },   { ".doc", "DOC" },   { ".docx", "DOCX" },
};

/* Номер формата в formats или -1 — не книга. */
static int book_format(const char *name)
{
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof formats / sizeof *formats; i++) {
        size_t el = strlen(formats[i].ext);
        if (n > el && !strcasecmp(name + n - el, formats[i].ext))
            return (int)i;
    }
    return -1;
}

/* Следующий символ UTF-8 без учёта регистра (латиница и кириллица, ё как е). */
static unsigned fold_next(const unsigned char **p)
{
    const unsigned char *s = *p;
    unsigned c = *s++;
    if (c >= 0xc0 && (*s & 0xc0) == 0x80) {
        int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        c &= 0x3f >> extra;
        while (extra-- > 0 && (*s & 0xc0) == 0x80)
            c = (c << 6) | (*s++ & 0x3f);
    }
    *p = s;
    if (c >= 'A' && c <= 'Z')
        return c + 32;
    if (c >= 0x410 && c <= 0x42f)
        return c + 32;
    if (c == 0x401 || c == 0x451)
        return 0x435;
    return c;
}

/* Сравнение имён: без учёта регистра, числа по значению («Книга 2» раньше «Книга 10»). */
static int name_cmp(const char *a, const char *b)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    while (*p && *q) {
        if (*p >= '0' && *p <= '9' && *q >= '0' && *q <= '9') {
            while (*p == '0')
                p++;
            while (*q == '0')
                q++;
            const unsigned char *p0 = p, *q0 = q;
            while (*p >= '0' && *p <= '9')
                p++;
            while (*q >= '0' && *q <= '9')
                q++;
            if (p - p0 != q - q0)
                return (int)(p - p0) - (int)(q - q0);
            int c = memcmp(p0, q0, (size_t)(p - p0));
            if (c)
                return c;
            continue;
        }
        unsigned x = fold_next(&p), y = fold_next(&q);
        if (x != y)
            return x < y ? -1 : 1;
    }
    return *p ? 1 : *q ? -1 : strcmp(a, b);
}

static int dir_cmp(const void *a, const void *b)
{
    return name_cmp((*(lib_dir *const *)a)->name, (*(lib_dir *const *)b)->name);
}

static int file_cmp(const void *a, const void *b)
{
    return name_cmp(((const lib_file *)a)->name, ((const lib_file *)b)->name);
}

static void free_dir(lib_dir *d)
{
    if (!d)
        return;
    for (int i = 0; i < d->ndirs; i++)
        free_dir(d->dirs[i]);
    for (int i = 0; i < d->nfiles; i++) {
        free(d->files[i].name);
        free(d->files[i].title);
        free(d->files[i].path);
    }
    free(d->dirs);
    free(d->files);
    free(d->path);
    free(d);
}

/* Папки устройства и программ в корне, где книг нет. */
static int skip_at_root(const char *name)
{
    static const char *const names[] = { "system", "applications", "washi", "fonts", "screensavers",
                                         "System Volume Information" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcasecmp(name, names[i]))
            return 1;
    return 0;
}

static lib_dir *scan_dir(const char *path, lib_dir *parent, int depth, int *ndirs_seen)
{
    DIR *dp = opendir(path);
    if (!dp)
        return NULL;
    lib_dir *d = calloc(1, sizeof *d);
    if (!d || !(d->path = strdup(path))) {
        free(d);
        closedir(dp);
        return NULL;
    }
    const char *slash = strrchr(d->path, '/');
    d->name = slash && slash[1] ? slash + 1 : d->path;
    d->parent = parent;
    (*ndirs_seen)++;

    int cap_dirs = 0, cap_files = 0;
    struct dirent *e;
    char full[1024];
    while ((e = readdir(dp)) != NULL) {
        if (e->d_name[0] == '.' || (depth == 0 && skip_at_root(e->d_name)))
            continue;
        if (snprintf(full, sizeof full, "%s/%s", path, e->d_name) >= (int)sizeof full)
            continue;
        int is_dir = e->d_type == DT_DIR, is_reg = e->d_type == DT_REG;
        struct stat st;
        int have_st = 0;
        if ((e->d_type == DT_UNKNOWN || e->d_type == DT_LNK) && stat(full, &st) == 0) {
            have_st = 1;
            is_dir = S_ISDIR(st.st_mode);
            is_reg = S_ISREG(st.st_mode);
        }
        if (is_dir && depth < MAX_DEPTH) {
            lib_dir *child = scan_dir(full, d, depth + 1, ndirs_seen);
            if (!child || child->total == 0) {
                free_dir(child);
                continue;
            }
            if (d->ndirs == cap_dirs) {
                cap_dirs = cap_dirs ? cap_dirs * 2 : 8;
                lib_dir **nd = realloc(d->dirs, sizeof *nd * (size_t)cap_dirs);
                if (!nd) {
                    free_dir(child);
                    continue;
                }
                d->dirs = nd;
            }
            d->dirs[d->ndirs++] = child;
            d->total += child->total;
        } else if (is_reg && book_format(e->d_name) >= 0) {
            if (d->nfiles == cap_files) {
                cap_files = cap_files ? cap_files * 2 : 16;
                lib_file *nf = realloc(d->files, sizeof *nf * (size_t)cap_files);
                if (!nf)
                    continue;
                d->files = nf;
            }
            lib_file *f = &d->files[d->nfiles];
            f->path = strdup(full);
            f->fmt = book_format(e->d_name);
            f->name = strndup(e->d_name, strlen(e->d_name) - strlen(formats[f->fmt].ext));
            int ok_st = have_st || stat(full, &st) == 0;
            f->size = ok_st ? (long long)st.st_size : -1;
            f->mtime = ok_st ? (long long)st.st_mtime : 0;
            f->dir = d;
            f->title = NULL;
            if (f->path) {
                char title[512];
                home_book_title(f->path, title, sizeof title);
                f->title = strdup(title);
            }
            f->rank = INT_MAX;
            f->status = ST_NEW;
            f->pct = -1;
            f->page = 0;
            if (!f->path || !f->name || !f->title) {
                free(f->path);
                free(f->name);
                free(f->title);
                continue;
            }
            d->nfiles++;
            d->total++;
        }
    }
    closedir(dp);
    qsort(d->dirs, (size_t)d->ndirs, sizeof *d->dirs, dir_cmp);
    qsort(d->files, (size_t)d->nfiles, sizeof *d->files, file_cmp);
    return d;
}

/* Строка без учёта регистра и «ё» в кодовые точки. */
static int fold(const char *s, unsigned *out, int max)
{
    const unsigned char *p = (const unsigned char *)s;
    int n = 0;
    while (*p && n < max)
        out[n++] = fold_next(&p);
    return n;
}

static int contains(const char *hay, const unsigned *needle, int nn)
{
    unsigned h[512];
    int nh = fold(hay, h, 512);
    for (int i = 0; i + nn <= nh; i++) {
        int k = 0;
        while (k < nn && h[i + k] == needle[k])
            k++;
        if (k == nn)
            return 1;
    }
    return 0;
}

/* ---- Список книг ---- */

static int grow_items(library *l)
{
    if (l->nitems < l->cap)
        return 1;
    int cap = l->cap ? l->cap * 2 : 64;
    item *ni = realloc(l->items, sizeof *ni * (size_t)cap);
    if (!ni)
        return 0;
    l->items = ni;
    l->cap = cap;
    return 1;
}

static void add_item(library *l, item_kind kind, lib_dir *d, lib_file *f)
{
    if (!grow_items(l))
        return;
    item *it = &l->items[l->nitems++];
    it->kind = kind;
    it->dir = d;
    it->file = f;
}

/* Все книги дерева в один список. */
static void collect_flat(library *l, lib_dir *d, int *cap)
{
    for (int i = 0; i < d->nfiles; i++) {
        if (l->nflat == *cap) {
            *cap = *cap ? *cap * 2 : 256;
            lib_file **nf = realloc(l->flat, sizeof *nf * (size_t)*cap);
            if (!nf)
                return;
            l->flat = nf;
        }
        l->flat[l->nflat++] = &d->files[i];
    }
    for (int i = 0; i < d->ndirs; i++)
        collect_flat(l, d->dirs[i], cap);
}

static unsigned path_hash(const char *p)
{
    unsigned h = 2166136261u;
    for (; *p; p++)
        h = (h ^ (unsigned char)*p) * 16777619u;
    return h;
}

/* Что с книгами сейчас — по списку недавних (он меняется, пока читают). */
static void refresh_status(library *l)
{
    const positions *pos = l->pos;
    for (int i = 0; i < POS_HASH; i++)
        l->pos_hash[i] = -1;
    for (int i = 0; i < pos->count && i < POS_HASH / 2; i++) {
        unsigned h = path_hash(pos->items[i].path) & (POS_HASH - 1);
        while (l->pos_hash[h] >= 0)
            h = (h + 1) & (POS_HASH - 1);
        l->pos_hash[h] = (short)i;
    }
    for (int i = 0; i < l->nflat; i++) {
        lib_file *f = l->flat[i];
        f->rank = INT_MAX;
        f->status = ST_NEW;
        f->pct = -1;
        f->page = 0;
        for (unsigned h = path_hash(f->path) & (POS_HASH - 1); l->pos_hash[h] >= 0; h = (h + 1) & (POS_HASH - 1)) {
            const position *p = &pos->items[l->pos_hash[h]];
            if (strcmp(p->path, f->path))
                continue;
            f->rank = l->pos_hash[h];
            f->status = p->finished ? ST_DONE : ST_READING;
            f->page = p->page;
            f->pct = p->count > 0 ? (p->page + 1) * 100 / p->count : -1;
            break;
        }
    }
}

static int matches(const library *l, const lib_file *f)
{
    switch (l->filter) {
    case FILTER_READING: return f->status == ST_READING;
    case FILTER_NEW: return f->status == ST_NEW && f->fmt == 0; /* где читают остальные — не знаем */
    case FILTER_DONE: return f->status == ST_DONE;
    default: return 1;
    }
}

/* Сколько книг папки (с вложенными) подходит под фильтр. */
static int tally(library *l, lib_dir *d)
{
    int n = 0;
    for (int i = 0; i < d->nfiles; i++)
        n += matches(l, &d->files[i]);
    for (int i = 0; i < d->ndirs; i++)
        n += tally(l, d->dirs[i]);
    d->shown = n;
    return n;
}

static int sort_mode; /* для сравнений qsort (только поток интерфейса) */

static int title_cmp(const lib_file *a, const lib_file *b)
{
    return name_cmp(a->title, b->title);
}

static int added_cmp(const lib_file *a, const lib_file *b)
{
    return a->mtime < b->mtime ? 1 : a->mtime > b->mtime ? -1 : title_cmp(a, b);
}

static int book_cmp(const void *pa, const void *pb)
{
    const lib_file *a = *(lib_file *const *)pa, *b = *(lib_file *const *)pb;
    switch (sort_mode) {
    case SORT_RECENT:
        /* Открытые — по тому, как давно; остальные — новые файлы первыми. */
        if (a->rank != b->rank)
            return a->rank < b->rank ? -1 : 1;
        return added_cmp(a, b);
    case SORT_ADDED: return added_cmp(a, b);
    default: return title_cmp(a, b);
    }
}

static void build_items(library *l)
{
    double t0 = clock_ms();
    l->nitems = 0;
    if (!l->cur)
        return;
    refresh_status(l);
    sort_mode = l->sort;
    if (l->view == VIEW_FOLDERS && !l->searching) {
        lib_dir *d = l->cur;
        tally(l, d);
        for (int i = 0; i < d->ndirs; i++)
            if (d->dirs[i]->shown > 0)
                add_item(l, ITEM_DIR, d->dirs[i], NULL);
        int n = 0;
        lib_file **nt = realloc(l->tmp, sizeof *nt * (size_t)(d->nfiles ? d->nfiles : 1));
        if (!nt)
            return;
        l->tmp = nt;
        for (int i = 0; i < d->nfiles; i++)
            if (matches(l, &d->files[i]))
                l->tmp[n++] = &d->files[i];
        qsort(l->tmp, (size_t)n, sizeof *l->tmp, book_cmp);
        for (int i = 0; i < n; i++)
            add_item(l, ITEM_FILE, NULL, l->tmp[i]);
    } else {
        qsort(l->flat, (size_t)l->nflat, sizeof *l->flat, book_cmp);
        unsigned q[256];
        int nq = l->searching ? fold(l->query, q, 256) : 0;
        for (int i = 0; i < l->nflat; i++) {
            lib_file *f = l->flat[i];
            if (!matches(l, f) || (l->searching && (!nq || !contains(f->title, q, nq))))
                continue;
            add_item(l, ITEM_FILE, NULL, f);
        }
    }
    double ms = clock_ms() - t0;
    if (ms > 20)
        log_msg("библиотека: список %d из %d книг за %.0f ms", l->nitems, l->nflat, ms);
}

static lib_dir *find_dir(lib_dir *d, const char *path)
{
    if (!*path)
        return NULL;
    if (!strcmp(d->path, path))
        return d;
    for (int i = 0; i < d->ndirs; i++) {
        lib_dir *f = find_dir(d->dirs[i], path);
        if (f)
            return f;
    }
    return NULL;
}

static void scan(library *l)
{
    double t0 = clock_ms();
    int seen = 0;
    l->tree = scan_dir(l->root, NULL, 0, &seen);
    if (!l->tree) {
        log_msg("библиотека: не открыть %s", l->root);
        l->tree = calloc(1, sizeof *l->tree);
        if (!l->tree)
            return;
        l->tree->path = strdup(l->root);
        l->tree->name = l->tree->path ? l->tree->path : "";
    }
    int cap = 0;
    l->nflat = 0;
    collect_flat(l, l->tree, &cap);
    log_msg("библиотека: %s — %d книг, просмотрено папок %d, %.0f ms", l->root, l->tree->total, seen,
            clock_ms() - t0);
    l->cur = l->tree;
    /* После library_reload — в ту же папку (если она ещё есть). */
    lib_dir *keep = find_dir(l->tree, l->keep_dir);
    if (keep)
        l->cur = keep;
    l->keep_dir[0] = 0;
}

/* Папка книги относительно корня: «Русский / Конструирование». */
static void folder_of(const library *l, const lib_dir *d, char *out, size_t len)
{
    size_t rl = strlen(l->root);
    const char *rel = !strncmp(d->path, l->root, rl) ? d->path + rl : d->path;
    while (*rel == '/')
        rel++;
    ui_path_crumbs(rel, strlen(rel), out, len);
}

/* ---- Снаружи ---- */

static int pick(const char *value, const char *const *keys, int n, int def)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(value, keys[i]))
            return i;
    return def;
}

library *library_new(const char *root, gfx *g, const positions *pos, covers *cv, const char *config_path)
{
    library *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    snprintf(l->root, sizeof l->root, "%s", root);
    size_t n = strlen(l->root);
    while (n > 1 && l->root[n - 1] == '/')
        l->root[--n] = 0;
    snprintf(l->config_path, sizeof l->config_path, "%s", config_path ? config_path : "");
    l->g = g;
    l->pos = pos;
    l->cv = cv;
    config cfg;
    if (*l->config_path && config_load(&cfg, l->config_path) == 0) {
        l->view = pick(config_get(&cfg, "lib_view", ""), view_keys, VIEW_COUNT, VIEW_GRID);
        l->sort = pick(config_get(&cfg, "lib_sort", ""), sort_keys, SORT_COUNT, SORT_RECENT);
    }
    return l;
}

void library_free(library *l)
{
    if (!l)
        return;
    free_dir(l->tree);
    free(l->items);
    free(l->flat);
    free(l->tmp);
    free(l);
}

void library_reload(library *l)
{
    snprintf(l->keep_dir, sizeof l->keep_dir, "%s", l->cur && l->cur->parent ? l->cur->path : "");
    free_dir(l->tree);
    free(l->flat);
    l->tree = l->cur = NULL;
    l->flat = NULL;
    l->nflat = l->nitems = l->ncells = 0;
    l->searching = 0;
}

static int content_top(void)
{
    return ui_content_top() + FILTER_ROW_H;
}

/* Галочка-уголок вниз у «Недавние ⌄». */
static void chevron_down(gfx *g, pf_screen *s, int x, int y, int size, uint8_t gray)
{
    static const float vb[4] = { 0, 0, 24, 24 };
    gfx_svg(g, s, "M6 9L12 15L18 9", vb, x, y, size, 2.5f, gray);
}

static const gfx_icon_id view_icons[VIEW_COUNT] = { ICON_APPS, ICON_TOC, ICON_FOLDER };

/* Фильтры слева; справа — сортировка, вид и поиск. */
static void draw_filters(library *l, pf_screen *s)
{
    int y = ui_content_top() + 30, x = UI_MARGIN;
    for (int i = 0; i < FILTER_COUNT; i++) {
        int on = l->filter == i;
        gfx_style st = { on ? FONT_BOLD : FONT_REGULAR, 36, on ? UI_BLACK : UI_TEXT3 };
        int w = gfx_text_width(l->g, st, filter_names[i]);
        gfx_text(l->g, s, x, y, st, filter_names[i]);
        if (on)
            gfx_rrect(l->g, s, x, y + 16, w, 6, 3, UI_BLACK);
        l->filter_hits[i] = (ui_rect){ x - 22, y - 50, w + 44, 86 };
        x += w + 48;
    }
    int isz = 56, sx = s->width - UI_MARGIN - isz;
    gfx_icon(l->g, s, ICON_SEARCH, sx, y - 44, isz, 0, l->searching ? UI_BLACK : UI_TEXT2);
    l->search_hit = (ui_rect){ sx - 20, y - 60, s->width - sx + 20, 90 };

    int vsz = 52, vx = sx - 44 - vsz;
    gfx_icon(l->g, s, view_icons[l->view], vx, y - 42, vsz, 0, UI_TEXT2);
    gfx_style ss = { FONT_REGULAR, 33, UI_TEXT2 };
    const char *sort = sort_names[l->sort];
    int sw = gfx_text_width(l->g, ss, sort), cx = vx - 36 - 30, tx = cx - 6 - sw;
    gfx_text(l->g, s, tx, y - 2, ss, sort);
    chevron_down(l->g, s, cx, y - 30, 30, UI_TEXT2);
    l->sort_hit = (ui_rect){ tx - 20, y - 60, cx + 30 - tx + 20, 90 };
    l->view_hit = (ui_rect){ vx - 20, y - 60, vsz + 40, 90 };
    gfx_fill(s, 0, y + 30, s->width, 2, UI_FAINT);
}

static void status_text(const lib_file *f, char *out, size_t len)
{
    if (f->status == ST_DONE)
        snprintf(out, len, "прочитана");
    else if (f->status == ST_READING && f->pct >= 0)
        snprintf(out, len, "%d %%", f->pct);
    else if (f->status == ST_READING)
        snprintf(out, len, "стр. %d", f->page + 1);
    else
        snprintf(out, len, "новая");
}

static cell *add_cell(library *l, ui_rect hit, int item)
{
    if (l->ncells >= MAX_CELLS)
        return NULL;
    cell *c = &l->cells[l->ncells++];
    memset(c, 0, sizeof *c);
    c->hit = hit;
    c->item = item;
    return c;
}

static void cover_in(library *l, pf_screen *s, cell *c, ui_rect box, int radius, const lib_file *f)
{
    int waiting = ui_book_cover(l->g, s, l->cv, box, radius, f->path, UI_WHITE);
    if (c) {
        c->cover = box;
        c->radius = radius;
        c->waiting = waiting;
    }
}

/* Сетка обложек: GRID_COLS × GRID_ROWS, рамки 3 : 4 по высоте ряда. */
typedef struct {
    int x0, y0, bw, ch, gap, row_h;
} grid_geo;

static grid_geo grid_layout(const pf_screen *s)
{
    grid_geo g;
    g.y0 = content_top() + 20;
    int avail = ui_content_bottom(s) - 8 - g.y0;
    g.row_h = (avail - (GRID_ROWS - 1) * GRID_ROW_GAP) / GRID_ROWS;
    g.ch = g.row_h - GRID_TEXT_H;
    g.bw = g.ch * 3 / 4;
    int maxw = (s->width - 2 * UI_MARGIN - (GRID_COLS - 1) * 32) / GRID_COLS;
    if (g.bw > maxw) {
        g.bw = maxw;
        g.ch = BOOKCOVER_H(maxw);
    }
    g.gap = (s->width - 2 * UI_MARGIN - GRID_COLS * g.bw) / (GRID_COLS - 1);
    g.x0 = UI_MARGIN;
    return g;
}

static void draw_grid_item(library *l, pf_screen *s, const grid_geo *g, int slot, int idx)
{
    const lib_file *f = l->items[idx].file;
    int col = slot % GRID_COLS, row = slot / GRID_COLS;
    int x = g->x0 + col * (g->bw + g->gap), y = g->y0 + row * (g->row_h + GRID_ROW_GAP);
    cell *c = add_cell(l, (ui_rect){ x - g->gap / 2, y - 10, g->bw + g->gap, g->row_h + 20 }, idx);
    cover_in(l, s, c, (ui_rect){ x, y, g->bw, g->ch }, 10, f);

    /* Под обложкой: название, ниже — прогресс (у новых — ничего). */
    gfx_style ts = { FONT_REGULAR, 29, UI_BLACK };
    gfx_text_fit(l->g, s, x, y + g->ch + 42, g->bw, ts, f->title);
    int open = !strcmp(f->path, l->current);
    gfx_style ps = { FONT_REGULAR, 26, UI_TEXT2 };
    if (f->status == ST_READING && f->pct >= 0) {
        char pct[16];
        snprintf(pct, sizeof pct, "%d %%", f->pct);
        int pw = gfx_text_width(l->g, ps, pct);
        ui_progress(l->g, s, x, y + g->ch + 68, g->bw - pw - 14, 6, f->pct / 100.0f);
        gfx_text(l->g, s, x + g->bw - pw, y + g->ch + 80, ps, pct);
    } else if (f->fmt != 0) {
        gfx_text(l->g, s, x, y + g->ch + 80, ps, formats[f->fmt].label);
    } else if (f->status != ST_NEW || open) {
        char st[32];
        status_text(f, st, sizeof st);
        gfx_text(l->g, s, x, y + g->ch + 80, ps, open && f->status == ST_NEW ? "открыта" : st);
    }
}

/* Строка списка: обложка (или значок папки), название, подпись, «›» у папки. */
static void draw_row(library *l, pf_screen *s, int y, int idx, int line)
{
    const item *it = &l->items[idx];
    int x = UI_MARGIN, right = s->width - UI_MARGIN, th = BOOKCOVER_H(LIST_THUMB_W);
    cell *c = add_cell(l, (ui_rect){ 0, y, s->width, UI_ROW_H }, idx);
    char sub[600];
    const char *title;
    if (it->kind == ITEM_DIR) {
        gfx_icon(l->g, s, ICON_FOLDER, x + (LIST_THUMB_W - 60) / 2, y + (UI_ROW_H - 60) / 2, 60, 0, UI_BLACK);
        int n = l->filter == FILTER_ALL ? it->dir->total : it->dir->shown;
        snprintf(sub, sizeof sub, "%d %s", n, ui_plural(n, "книга", "книги", "книг"));
        title = it->dir->name;
        gfx_icon(l->g, s, ICON_NEXT, right - 44, y + UI_ROW_H / 2 - 22, 44, 0, UI_TEXT3);
        right -= 64;
    } else {
        const lib_file *f = it->file;
        cover_in(l, s, c, (ui_rect){ x, y + (UI_ROW_H - th) / 2, LIST_THUMB_W, th }, 6, f);
        char st[32], where[400] = "";
        if (f->fmt == 0)
            status_text(f, st, sizeof st);
        else
            snprintf(st, sizeof st, "%s", formats[f->fmt].label); /* читает штатная читалка */
        if (l->view != VIEW_FOLDERS || l->searching)
            folder_of(l, f->dir, where, sizeof where);
        if (!*where)
            ui_format_size(where, sizeof where, f->size);
        snprintf(sub, sizeof sub, "%s%s%s%s", !strcmp(f->path, l->current) ? "открыта · " : "", st,
                 *where ? " · " : "", where);
        title = f->title;
    }
    int tx = x + LIST_THUMB_W + 36;
    gfx_style ts = { FONT_REGULAR, 40, UI_BLACK }, ss = { FONT_REGULAR, 30, UI_TEXT2 };
    gfx_text_fit(l->g, s, tx, y + 64, right - tx, ts, title);
    gfx_text_fit(l->g, s, tx, y + 108, right - tx, ss, sub);
    if (line)
        gfx_fill(s, tx, y + UI_ROW_H - 2, s->width - UI_MARGIN - tx, 2, UI_LINE);
}

/* Лист «Показывать»: вид и сортировка пилюлями, выбранное — чёрное. */
static void draw_sheet(library *l, pf_screen *s)
{
    int W = s->width, M = UI_MARGIN, y0 = ui_content_bottom(s) - SHEET_H;
    gfx_fill(s, 0, y0, W, SHEET_H, UI_WHITE);
    gfx_fill(s, 0, y0, W, 2, UI_TEXT3);
    gfx_style ts = { FONT_BOLD, 44, UI_BLACK };
    gfx_text(l->g, s, M, y0 + 84, ts, "Показывать");
    int gap = 16, bw = (W - 2 * M - 2 * gap) / 3, bh = 100;
    ui_caption(l->g, s, M, y0 + 160, "Вид");
    for (int i = 0; i < VIEW_COUNT; i++) {
        int x = M + i * (bw + gap), y = y0 + 186;
        ui_pill(l->g, s, x, y, bw, bh, view_names[i], i == l->view);
        l->sheet_view[i] = (ui_rect){ x, y - 10, bw, bh + 20 };
    }
    ui_caption(l->g, s, M, y0 + 352, "Сортировка");
    for (int i = 0; i < SORT_COUNT; i++) {
        int x = M + i * (bw + gap), y = y0 + 378;
        ui_pill(l->g, s, x, y, bw, bh, sort_names[i], i == l->sort);
        l->sheet_sort[i] = (ui_rect){ x, y - 10, bw, bh + 20 };
    }
}

static const char *empty_text(const library *l)
{
    if (l->searching)
        return "Ничего не нашлось.";
    if (l->nflat == 0)
        return "Книг не найдено. Скопируйте книги на устройство или загрузите по сети: «Приложения» → «Приём по Wi-Fi» или «Каталоги».";
    switch (l->filter) {
    case FILTER_READING: return "Начатых книг нет.";
    case FILTER_NEW: return "Все книги уже открывали.";
    case FILTER_DONE: return "Прочитанных книг пока нет.";
    default: return "Здесь книг нет.";
    }
}

void library_draw(library *l, pf_screen *s, const char *current)
{
    snprintf(l->current, sizeof l->current, "%s", current ? current : "");
    if (!l->tree)
        scan(l);
    if (!l->cur)
        return;
    build_items(l);
    int top = content_top(), bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_WHITE);

    int grid = l->view == VIEW_GRID;
    l->per_page = grid ? GRID_COLS * GRID_ROWS : (bottom - top) / UI_ROW_H;
    if (l->per_page < 1)
        l->per_page = 1;
    if (l->per_page > MAX_CELLS)
        l->per_page = MAX_CELLS;
    l->pages = l->nitems ? (l->nitems + l->per_page - 1) / l->per_page : 1;
    if (l->page >= l->pages)
        l->page = l->pages - 1;
    if (l->page < 0)
        l->page = 0;

    char title[320];
    int back = 0;
    if (l->searching) {
        back = 1;
        snprintf(title, sizeof title, "«%s» · %d", l->query, l->nitems);
    } else if (l->view == VIEW_FOLDERS && l->cur->parent) {
        back = 1;
        snprintf(title, sizeof title, "%s", l->cur->name);
    } else {
        int n = l->nitems;
        if (l->view == VIEW_FOLDERS)
            n = l->filter == FILTER_ALL ? l->tree->total : l->tree->shown;
        snprintf(title, sizeof title, "Библиотека · %d", n);
    }
    l->hits = ui_header(l->g, s, title, back, l->page, l->pages);
    draw_filters(l, s);

    l->ncells = 0;
    int first = l->page * l->per_page;
    if (grid) {
        grid_geo g = grid_layout(s);
        for (int i = 0; i < l->per_page && first + i < l->nitems; i++)
            draw_grid_item(l, s, &g, i, first + i);
    } else {
        for (int i = 0; i < l->per_page && first + i < l->nitems; i++) {
            int last = first + i + 1 >= l->nitems || i + 1 >= l->per_page;
            draw_row(l, s, top + i * UI_ROW_H, first + i, !last);
        }
    }
    if (l->nitems == 0) {
        gfx_style st = { FONT_REGULAR, 40, UI_TEXT2 };
        gfx_text_wrap(l->g, s, UI_MARGIN, top + 100, s->width - 2 * UI_MARGIN, 52, 3, st, empty_text(l));
    }
    if (l->sheet)
        draw_sheet(l, s);
}

int library_covers_ready(library *l, pf_screen *s)
{
    int waiting = 0;
    for (int i = 0; i < l->ncells; i++) {
        cell *c = &l->cells[i];
        if (!c->waiting)
            continue;
        const cover_info *ci = NULL;
        if (covers_get(l->cv, l->items[c->item].file->path, &ci) == 1) {
            waiting++;
            continue;
        }
        cover_in(l, s, c, c->cover, c->radius, l->items[c->item].file);
        if (l->sheet && c->cover.y + c->cover.h > ui_content_bottom(s) - SHEET_H)
            continue; /* под листом — покажется, когда его закроют */
        pf_update(c->cover.x, c->cover.y, c->cover.w, c->cover.h, PF_UPDATE_PARTIAL);
    }
    return waiting;
}

static int turn(library *l, int delta)
{
    int p = l->page + delta;
    if (p < 0 || p >= l->pages)
        return 0;
    l->page = p;
    return 1;
}

static int go_up(library *l)
{
    if (l->sheet) {
        l->sheet = 0;
        return 1;
    }
    if (l->searching) {
        l->searching = 0;
        l->page = 0;
        return 1;
    }
    if (l->view != VIEW_FOLDERS || !l->cur || !l->cur->parent)
        return 0;
    lib_dir *from = l->cur;
    l->cur = l->cur->parent;
    build_items(l);
    /* Вернуться на страницу, где была папка, из которой вышли. */
    l->page = 0;
    for (int i = 0; i < l->nitems; i++)
        if (l->items[i].dir == from && l->per_page > 0) {
            l->page = i / l->per_page;
            break;
        }
    return 1;
}

library_action library_key(library *l, pf_key key)
{
    if (l->sheet) {
        l->sheet = 0;
        return LIBRARY_REDRAW;
    }
    if (key == PF_KEY_NEXT)
        return turn(l, 1) ? LIBRARY_REDRAW : LIBRARY_NONE;
    if (key == PF_KEY_PREV)
        return turn(l, -1) ? LIBRARY_REDRAW : LIBRARY_NONE;
    return LIBRARY_NONE;
}

static void save_pref(library *l, const char *key, const char *value)
{
    if (*l->config_path && config_save_value(l->config_path, key, value) != 0)
        log_msg("библиотека: не удалось записать %s в %s", key, l->config_path);
}

static library_action sheet_tap(library *l, int x, int y)
{
    l->sheet = 0;
    for (int i = 0; i < VIEW_COUNT; i++)
        if (ui_inside(l->sheet_view[i], x, y) && i != l->view) {
            l->view = i;
            l->page = 0;
            save_pref(l, "lib_view", view_keys[i]);
            log_msg("библиотека: вид %s", view_keys[i]);
        }
    for (int i = 0; i < SORT_COUNT; i++)
        if (ui_inside(l->sheet_sort[i], x, y) && i != l->sort) {
            l->sort = i;
            l->page = 0;
            save_pref(l, "lib_sort", sort_keys[i]);
            log_msg("библиотека: сортировка %s", sort_keys[i]);
        }
    return LIBRARY_REDRAW; /* и мимо листа — закрыть его */
}

static const cell *cell_at(const library *l, int x, int y)
{
    for (int i = 0; i < l->ncells; i++) {
        const cell *c = &l->cells[i];
        if (ui_inside(c->hit, x, y) && c->item < l->nitems)
            return c;
    }
    return NULL;
}

library_action library_long(library *l, int x, int y)
{
    if (!l->cur || l->sheet)
        return library_tap(l, x, y);
    const cell *c = cell_at(l, x, y);
    if (c && l->items[c->item].kind == ITEM_DIR) {
        const lib_dir *d = l->items[c->item].dir;
        snprintf(l->folder, sizeof l->folder, "%s", d->path);
        snprintf(l->folder_name, sizeof l->folder_name, "%s", d->name);
        l->folder_books = d->total;
        l->selected_hit = c->hit;
        return LIBRARY_FOLDER;
    }
    return library_tap(l, x, y);
}

const char *library_folder(const library *l)
{
    return l->folder;
}

const char *library_folder_name(const library *l)
{
    return l->folder_name;
}

int library_folder_books(const library *l)
{
    return l->folder_books;
}

library_action library_tap(library *l, int x, int y)
{
    if (!l->cur)
        return LIBRARY_NONE;
    if (l->sheet)
        return sheet_tap(l, x, y);
    if (ui_inside(l->hits.back, x, y))
        return go_up(l) ? LIBRARY_REDRAW : LIBRARY_NONE;
    if (ui_inside(l->hits.prev, x, y))
        return turn(l, -1) ? LIBRARY_REDRAW : LIBRARY_NONE;
    if (ui_inside(l->hits.next, x, y))
        return turn(l, 1) ? LIBRARY_REDRAW : LIBRARY_NONE;
    if (ui_inside(l->search_hit, x, y))
        return LIBRARY_SEARCH;
    if (ui_inside(l->sort_hit, x, y) || ui_inside(l->view_hit, x, y)) {
        l->sheet = 1;
        return LIBRARY_REDRAW;
    }
    for (int i = 0; i < FILTER_COUNT; i++)
        if (ui_inside(l->filter_hits[i], x, y)) {
            if (l->filter == i)
                return LIBRARY_NONE;
            l->filter = i;
            l->page = 0;
            return LIBRARY_REDRAW;
        }
    for (int i = 0; i < l->ncells; i++) {
        const cell *c = &l->cells[i];
        if (!ui_inside(c->hit, x, y) || c->item >= l->nitems)
            continue;
        const item *it = &l->items[c->item];
        if (it->kind == ITEM_DIR) {
            l->cur = it->dir;
            l->page = 0;
            return LIBRARY_REDRAW;
        }
        snprintf(l->selected, sizeof l->selected, "%s", it->file->path);
        l->selected_hit = c->hit;
        log_msg("библиотека: открыть %s", it->file->path);
        return LIBRARY_OPEN;
    }
    return LIBRARY_NONE;
}

void library_search(library *l, const char *query)
{
    if (!query || !*query)
        return;
    snprintf(l->query, sizeof l->query, "%s", query);
    l->searching = 1;
    l->filter = FILTER_ALL; /* ищут среди всех книг */
    l->sheet = 0;
    l->page = 0;
    build_items(l);
    log_msg("поиск «%s»: %d книг", query, l->nitems);
}

const char *library_query(const library *l)
{
    return l->query;
}

int library_back(library *l)
{
    return go_up(l);
}

void library_close_sheet(library *l)
{
    l->sheet = 0;
}

const char *library_selected(const library *l)
{
    return l->selected;
}

ui_rect library_hit(const library *l)
{
    return l->selected_hit;
}
