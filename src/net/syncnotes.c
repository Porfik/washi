#include "syncnotes.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../base/log.h"
#include "../base/util.h"
#include "../base/bookmarks.h"

void notes_key(const char *sync_dir, const char *path, char *key, size_t len)
{
    size_t n = strlen(sync_dir);
    if (n && !strncmp(path, sync_dir, n) && path[n] == '/' && path[n + 1])
        snprintf(key, len, "%s", path + n + 1);
    else
        snprintf(key, len, "%s", path);
}

void notes_path(const char *sync_dir, const char *key, char *path, size_t len)
{
    if (key[0] == '/')
        snprintf(path, len, "%s", key);
    else if (strlen(sync_dir) + 1 + strlen(key) < len)
        snprintf(path, len, "%s/%s", sync_dir, key);
    else
        path[0] = 0; /* не влезает — такого файла нет */
}

/* ---- Файл закладок целиком ---- */

typedef struct {
    char *key;
    int *pages; /* с 0, по возрастанию */
    int n;
} bm_entry;

typedef struct {
    bm_entry *v;
    int n, cap;
} bm_file;

#define BM_MAX BOOKMARKS_MAX
#define BM_FILE_MAX 4000

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* «1,5,9» → страницы с 0 (отсортированы, без повторов); malloc, *n — сколько. */
static int *parse_pages(const char *list, int *n)
{
    int *v = malloc(sizeof *v * BM_MAX);
    *n = 0;
    if (!v)
        return NULL;
    for (const char *p = list; *p && *n < BM_MAX;) {
        int x = atoi(p);
        if (x > 0)
            v[(*n)++] = x - 1;
        p = strchr(p, ',');
        if (!p)
            break;
        p++;
    }
    qsort(v, (size_t)*n, sizeof *v, cmp_int);
    int m = 0;
    for (int i = 0; i < *n; i++)
        if (!m || v[m - 1] != v[i])
            v[m++] = v[i];
    *n = m;
    return v;
}

static void bm_free(bm_file *f)
{
    for (int i = 0; i < f->n; i++) {
        free(f->v[i].key);
        free(f->v[i].pages);
    }
    free(f->v);
    memset(f, 0, sizeof *f);
}

static const bm_entry *bm_find(const bm_file *f, const char *key)
{
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->v[i].key, key))
            return &f->v[i];
    return NULL;
}

/* Добавить или заменить запись; pages копируются. */
static int bm_set(bm_file *f, const char *key, const int *pages, int n)
{
    bm_entry *e = NULL;
    for (int i = 0; i < f->n; i++)
        if (!strcmp(f->v[i].key, key))
            e = &f->v[i];
    if (!e) {
        if (f->n >= BM_FILE_MAX)
            return 0;
        if (f->n == f->cap) {
            int cap = f->cap ? f->cap * 2 : 64;
            bm_entry *v = realloc(f->v, sizeof *v * (size_t)cap);
            if (!v)
                return -1;
            f->v = v;
            f->cap = cap;
        }
        e = &f->v[f->n];
        memset(e, 0, sizeof *e);
        e->key = strdup(key);
        if (!e->key)
            return -1;
        f->n++;
    }
    int *copy = malloc(sizeof *copy * (size_t)(n ? n : 1));
    if (!copy)
        return -1;
    memcpy(copy, pages, sizeof *copy * (size_t)n);
    free(e->pages);
    e->pages = copy;
    e->n = n;
    return 0;
}

/* Строки «СТР,СТР<TAB>ключ». Нет файла — пусто. */
static void bm_load(bm_file *f, const char *file)
{
    FILE *in = fopen(file, "r");
    if (!in)
        return;
    char line[4600];
    while (fgets(line, sizeof line, in)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (!tab || !tab[1])
            continue;
        *tab = 0;
        int n;
        int *v = parse_pages(line, &n);
        if (v && n)
            bm_set(f, tab + 1, v, n);
        free(v);
    }
    fclose(in);
}

static int bm_save(const bm_file *f, const char *file)
{
    FILE *o = file_save_begin(file);
    if (!o)
        return -1;
    for (int i = 0; i < f->n; i++) {
        if (!f->v[i].n)
            continue;
        for (int k = 0; k < f->v[i].n; k++)
            fprintf(o, "%s%d", k ? "," : "", f->v[i].pages[k] + 1);
        fprintf(o, "\t%s\n", f->v[i].key);
    }
    return file_save_end(o, file, 1);
}

/* ---- База ---- */

typedef struct {
    char *key;
    int bm;             /* 1 — закладки, 0 — место чтения */
    int page, finished; /* место чтения */
    int *pages, npages; /* закладки (с 0, по возрастанию) */
} base_item;

typedef struct {
    base_item *v;
    int n, cap;
} base;

static base_item *base_find(const base *b, const char *key, int bm)
{
    for (int i = 0; i < b->n; i++)
        if (b->v[i].bm == bm && !strcmp(b->v[i].key, key))
            return &b->v[i];
    return NULL;
}

static base_item *base_add(base *b, const char *key, int bm)
{
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        base_item *v = realloc(b->v, sizeof *v * (size_t)cap);
        if (!v)
            return NULL;
        b->v = v;
        b->cap = cap;
    }
    base_item *it = &b->v[b->n];
    memset(it, 0, sizeof *it);
    it->key = strdup(key);
    if (!it->key)
        return NULL;
    it->bm = bm;
    b->n++;
    return it;
}

static void base_free(base *b)
{
    for (int i = 0; i < b->n; i++) {
        free(b->v[i].key);
        free(b->v[i].pages);
    }
    free(b->v);
    memset(b, 0, sizeof *b);
}

/* Строки: «p<TAB>страница с 1<TAB>дочитана<TAB>ключ» и «b<TAB>1,5,9<TAB>ключ». */
static void base_load(base *b, const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    char line[4600];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'p' && line[1] == '\t') {
            char *t1 = line + 2, *t2 = strchr(t1, '\t');
            if (!t2)
                continue;
            char *key = strchr(t2 + 1, '\t');
            if (!key)
                continue;
            base_item *it = base_add(b, key + 1, 0);
            if (!it)
                break;
            it->page = atoi(t1) - 1;
            it->finished = atoi(t2 + 1);
        } else if (line[0] == 'b' && line[1] == '\t') {
            char *list = line + 2, *key = strchr(list, '\t');
            if (!key)
                continue;
            *key++ = 0;
            base_item *it = base_add(b, key, 1);
            if (!it)
                break;
            it->pages = parse_pages(list, &it->npages);
        }
    }
    fclose(f);
}

/* ---- Места чтения ---- */

static void copy_fields(position *dst, const position *src)
{
    char keep[sizeof dst->path];
    memcpy(keep, dst->path, sizeof keep);
    *dst = *src;
    memcpy(dst->path, keep, sizeof keep);
}

/* Слить запись книги (L) и сервера (S) с базой B (может быть NULL) → M. */
static void merge_pos(const position *L, const position *S, const base_item *B, position *M)
{
    int cl = !B || L->page != B->page, cs = !B || S->page != B->page;
    const position *win;
    if (cl && !cs)
        win = L;
    else if (cs && !cl)
        win = S;
    else
        win = S->page > L->page ? S : L;
    const position *other = win == L ? S : L;
    *M = *win;
    if (!M->count)
        M->count = other->count;
    /* «дочитана» не теряется: снимается, только если её сняли с одной стороны, а другая не менялась */
    int fl = L->finished, fs = S->finished;
    if (B && fl != B->finished && fs == B->finished)
        M->finished = fl;
    else if (B && fs != B->finished && fl == B->finished)
        M->finished = fs;
    else
        M->finished = fl || fs;
}

static int find_key(const positions *p, const char *key)
{
    for (int i = 0; i < p->count; i++)
        if (!strcmp(p->items[i].path, key))
            return i;
    return -1;
}

static int same_file(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    int same = 1;
    if (!fa || !fb) {
        /* нет серверного файла — одинаково, только если и слитое пусто */
        same = 0;
        if (fb && !fa) {
            fseek(fb, 0, SEEK_END);
            same = ftell(fb) == 0;
        }
    } else {
        int ca, cb;
        do {
            ca = fgetc(fa);
            cb = fgetc(fb);
        } while (ca == cb && ca != EOF);
        same = ca == cb;
    }
    if (fa)
        fclose(fa);
    if (fb)
        fclose(fb);
    return same;
}

static int merge_positions(positions *local, const notes_files *f, const base *bs, base *newb, notes_result *r)
{
    positions srv, up;
    memset(&up, 0, sizeof up);
    if (positions_load(&srv, f->srv_pos) < 0)
        return -1;
    snprintf(up.file, sizeof up.file, "%s", f->up_pos);
    up.items = calloc(POSITIONS_MAX, sizeof *up.items);
    if (!up.items) {
        positions_free(&srv);
        return -1;
    }
    int changed = 0, rc = 0;
    int nlocal = local->count; /* импортированные добавляются в конец, их не обходим */
    for (int i = 0; i < nlocal && up.count < POSITIONS_MAX; i++) {
        position *L = &local->items[i];
        position tmp = *L;
        notes_key(f->sync_dir, L->path, tmp.path, sizeof tmp.path);
        int si = find_key(&srv, tmp.path);
        position M = tmp;
        if (si >= 0) {
            const base_item *B = base_find(bs, tmp.path, 0);
            merge_pos(&tmp, &srv.items[si], B, &M);
            if (M.page != L->page || M.finished != L->finished)
                r->pos_updated++;
            if (M.page != L->page || M.finished != L->finished || M.count != L->count || M.view != L->view ||
                M.orient != L->orient || M.spread != L->spread || M.contrast != L->contrast ||
                M.has_crop != L->has_crop) {
                copy_fields(L, &M);
                changed = 1;
            }
        }
        up.items[up.count++] = M;
        base_item *nb = base_add(newb, M.path, 0);
        if (!nb) {
            rc = -1;
            break;
        }
        nb->page = M.page;
        nb->finished = M.finished;
    }
    /* то, что есть только на сервере */
    for (int i = 0; rc == 0 && i < srv.count && up.count < POSITIONS_MAX; i++) {
        const position *S = &srv.items[i];
        if (find_key(&up, S->path) >= 0)
            continue;
        char path[1024];
        notes_path(f->sync_dir, S->path, path, sizeof path);
        up.items[up.count++] = *S;
        base_item *nb = base_add(newb, S->path, 0);
        if (!nb) {
            rc = -1;
            break;
        }
        nb->page = S->page;
        nb->finished = S->finished;
        if (path[0] && access(path, F_OK) == 0 && local->count < POSITIONS_MAX) {
            position *L = &local->items[local->count++];
            *L = *S;
            snprintf(L->path, sizeof L->path, "%s", path);
            r->pos_imported++;
            changed = 1;
        }
    }
    if (rc == 0 && changed && positions_save(local) != 0)
        log_msg("синхронизация: не записать %s", local->file);
    if (rc == 0 && positions_save(&up) != 0)
        rc = -1;
    if (rc == 0)
        r->send_pos = !same_file(f->srv_pos, f->up_pos);
    positions_free(&srv);
    free(up.items);
    return rc;
}

/* ---- Закладки ---- */

static int has_page(const int *v, int n, int page)
{
    for (int i = 0; i < n; i++)
        if (v[i] == page)
            return 1;
    return 0;
}

static int merge_bookmarks(const notes_files *f, const base *bs, base *newb, notes_result *r)
{
    bm_file loc, srv, out;
    memset(&loc, 0, sizeof loc);
    memset(&srv, 0, sizeof srv);
    memset(&out, 0, sizeof out);
    bm_file raw;
    memset(&raw, 0, sizeof raw);
    bm_load(&raw, f->loc_bm);
    bm_load(&srv, f->srv_bm);
    int rc = 0, changed = 0;

    /* закладки книги — под портативными ключами */
    for (int i = 0; i < raw.n && rc == 0; i++) {
        char key[1100];
        notes_key(f->sync_dir, raw.v[i].key, key, sizeof key);
        rc = bm_set(&loc, key, raw.v[i].pages, raw.v[i].n);
    }
    bm_free(&raw);

    int nkeys = loc.n + srv.n;
    for (int k = 0; k < nkeys && rc == 0; k++) {
        const char *key = k < loc.n ? loc.v[k].key : srv.v[k - loc.n].key;
        if (k >= loc.n && bm_find(&loc, key))
            continue;
        const bm_entry *L = bm_find(&loc, key), *S = bm_find(&srv, key);
        const base_item *B = base_find(bs, key, 1);
        char path[1100];
        notes_path(f->sync_dir, key, path, sizeof path);
        int here = L != NULL || access(path, F_OK) == 0;
        int cand[BM_MAX * 2], n = 0;
        if (!here) {
            for (int i = 0; S && i < S->n; i++)
                cand[n++] = S->pages[i]; /* книги нет на устройстве: сервер остаётся как был */
        } else {
            for (int pass = 0; pass < 2; pass++) {
                const bm_entry *E = pass ? S : L;
                for (int i = 0; E && i < E->n; i++) {
                    int p = E->pages[i];
                    int removed = B && has_page(B->pages, B->npages, p) &&
                                  (!L || !has_page(L->pages, L->n, p) || !S || !has_page(S->pages, S->n, p));
                    if (!removed)
                        cand[n++] = p; /* снятая с одной стороны относительно базы — снимается и с другой */
                }
            }
        }
        qsort(cand, (size_t)n, sizeof *cand, cmp_int);
        int m = 0;
        for (int i = 0; i < n; i++)
            if (!m || cand[m - 1] != cand[i])
                cand[m++] = cand[i];
        if (m > BM_MAX)
            m = BM_MAX;
        if (here && !(L && L->n == m && !memcmp(L->pages, cand, sizeof(int) * (size_t)m))) {
            changed = 1;
            r->bm_updated++;
        }
        if (!m)
            continue;
        rc = bm_set(&out, key, cand, m);
        base_item *nb = rc == 0 ? base_add(newb, key, 1) : NULL;
        if (rc == 0 && (!nb || !(nb->pages = malloc(sizeof(int) * (size_t)m)))) {
            rc = -1;
            break;
        }
        if (rc == 0) {
            memcpy(nb->pages, cand, sizeof(int) * (size_t)m);
            nb->npages = m;
        }
    }
    if (rc == 0 && changed) {
        /* файл книги: книги, что есть на устройстве, — со слитыми закладками */
        bm_file mine;
        memset(&mine, 0, sizeof mine);
        for (int i = 0; i < out.n && rc == 0; i++) {
            char path[1100];
            notes_path(f->sync_dir, out.v[i].key, path, sizeof path);
            if (bm_find(&loc, out.v[i].key) || access(path, F_OK) == 0)
                rc = bm_set(&mine, path, out.v[i].pages, out.v[i].n);
        }
        if (rc == 0 && bm_save(&mine, f->loc_bm) != 0)
            log_msg("синхронизация: не записать %s", f->loc_bm);
        bm_free(&mine);
    }
    if (rc == 0 && bm_save(&out, f->up_bm) != 0)
        rc = -1;
    if (rc == 0)
        r->send_bm = !same_file(f->srv_bm, f->up_bm);
    bm_free(&loc);
    bm_free(&srv);
    bm_free(&out);
    return rc;
}

/* ---- Всё вместе ---- */

int notes_merge(positions *local, const notes_files *f, notes_result *r)
{
    memset(r, 0, sizeof *r);
    base bs = { NULL, 0, 0 }, nb = { NULL, 0, 0 };
    base_load(&bs, f->base);
    int rc = merge_positions(local, f, &bs, &nb, r);
    if (rc == 0)
        rc = merge_bookmarks(f, &bs, &nb, r);
    if (rc == 0) {
        FILE *o = file_save_begin(f->new_base);
        if (!o) {
            rc = -1;
        } else {
            for (int i = 0; i < nb.n; i++) {
                const base_item *it = &nb.v[i];
                if (it->pages) {
                    fprintf(o, "b\t");
                    for (int k = 0; k < it->npages; k++)
                        fprintf(o, "%s%d", k ? "," : "", it->pages[k] + 1);
                    fprintf(o, "\t%s\n", it->key);
                } else {
                    fprintf(o, "p\t%d\t%d\t%s\n", it->page + 1, it->finished, it->key);
                }
            }
            if (file_save_end(o, f->new_base, 0) != 0)
                rc = -1;
        }
    }
    base_free(&bs);
    base_free(&nb);
    return rc;
}
