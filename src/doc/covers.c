#define _GNU_SOURCE /* pthread_timedjoin_np */
#include "covers.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../base/clock.h"
#include "../base/log.h"
#include "../base/util.h"

#ifndef COVERS_JOIN_S
#define COVERS_JOIN_S 2
#endif

typedef enum { C_NEW, C_QUEUED, C_WORKING, C_READY, C_FAILED } cover_state;

typedef struct {
    char *path;
    char key[17];
    cover_state state;
    unsigned seq;  /* когда запрошена последний раз: больше — раньше рисовать */
    unsigned used; /* когда отдана последний раз: давно не нужные выгружаются из памяти */
    uint8_t *img;
    cover_info info;
} entry;

struct covers {
    char dir[1024];
    doc_options opt;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    entry **items;
    int n, cap;
    unsigned seq, use_seq;
    int nimg; /* обложек в памяти */
    int ready, paused, quit;
    pthread_t thread;
    int started;
    doc_cookie *cookie;
};

/* ---- Диск ---- */

static void make_key(const char *path, const struct stat *st, char out[17])
{
    unsigned long long h = 1469598103934665603ull;
    char extra[64];
    snprintf(extra, sizeof extra, "|%lld|%lld", (long long)st->st_size, (long long)st->st_mtime);
    for (const char *p = path; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    for (const char *p = extra; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    snprintf(out, 17, "%016llx", h);
}

/* Метка «обложка этой книги в работе»: если процесс умрёт или зависнет на файле (и его придётся
 * перезагрузить), при следующем запуске книга пропускается — иначе хаб падал бы при каждом старте.
 * Метка живёт, пока не изменятся размер или время файла (ключ). */
static void mark_path(const covers *c, const char *key, char *out, size_t len)
{
    snprintf(out, len, "%s/%s.try", c->dir, key);
}

static void field(char **p, char *out, size_t len)
{
    char *e = strchr(*p, '\t');
    size_t n = e ? (size_t)(e - *p) : strcspn(*p, "\r\n");
    if (n >= len)
        n = len - 1;
    memcpy(out, *p, n);
    out[n] = 0;
    *p = e ? e + 1 : *p + strlen(*p);
}

/* Обложка и сведения с диска; 0 — есть. Сведения без обложки (не нарисовалась) — тоже 0. */
static int load_cached(covers *c, entry *e)
{
    char path[1100], line[600];
    snprintf(path, sizeof path, "%s/%s.inf", c->dir, e->key);
    FILE *f = fopen(path, "r");
    if (!f) {
        /* Прежние сведения были .txt — их индексатор прошивки считал книгами. */
        char old[1100];
        snprintf(old, sizeof old, "%s/%s.txt", c->dir, e->key);
        if (rename(old, path) != 0 || !(f = fopen(path, "r")))
            return -1;
    }
    int ok = fgets(line, sizeof line, f) != NULL;
    fclose(f);
    if (!ok)
        return -1;
    char *p = line, num[32], has[8];
    field(&p, num, sizeof num);
    e->info.pages = atoi(num);
    field(&p, has, sizeof has);
    field(&p, e->info.author, sizeof e->info.author);
    field(&p, e->info.title, sizeof e->info.title);
    if (strcmp(has, "1"))
        return 0; /* обложки нет */

    snprintf(path, sizeof path, "%s/%s.pgm", c->dir, e->key);
    f = fopen(path, "rb");
    if (!f)
        return -1;
    int w = 0, h = 0, maxv = 0;
    if (fscanf(f, "P5 %d %d %d", &w, &h, &maxv) != 3 || w <= 0 || h <= 0 || w > COVER_W || h > COVER_H ||
        fgetc(f) == EOF) {
        fclose(f);
        return -1;
    }
    uint8_t *img = malloc((size_t)w * h);
    if (!img || fread(img, 1, (size_t)w * h, f) != (size_t)w * h) {
        free(img);
        fclose(f);
        return -1;
    }
    fclose(f);
    e->img = img;
    e->info.img = img;
    e->info.w = w;
    e->info.h = h;
    return 0;
}

/* Табуляции и переводы строк в сведениях PDF — пробелы: в файле по строке на книгу. */
static void clean(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static void save_cached(covers *c, const char *key, const cover_info *info)
{
    char path[1100];
    FILE *f;
    if (info->img) {
        snprintf(path, sizeof path, "%s/%s.pgm", c->dir, key);
        if ((f = file_save_begin(path))) {
            fprintf(f, "P5\n%d %d\n255\n", info->w, info->h);
            fwrite(info->img, 1, (size_t)info->w * info->h, f);
        }
        file_save_end(f, path, 0);
    }
    snprintf(path, sizeof path, "%s/%s.inf", c->dir, key);
    if ((f = file_save_begin(path)))
        fprintf(f, "%d\t%d\t%s\t%s\n", info->pages, info->img ? 1 : 0, info->author, info->title);
    file_save_end(f, path, 0);
}

/* ---- Поток отрисовки ---- */

/* 0 — готово, 1 — прервано (пауза), -1 — не вышло. */
static int render_cover(covers *c, const char *path, cover_info *info, uint8_t **img_out)
{
    char err[256];
    doc_options opt = c->opt;
    opt.store_bytes = (size_t)16 << 20;
    doc *d = doc_open(path, &opt, err, sizeof err);
    if (!d) {
        log_msg("обложка: %s: %s", path, err);
        return -1;
    }
    info->pages = doc_page_count(d);
    doc_meta(d, "info:Author", info->author, sizeof info->author);
    doc_meta(d, "info:Title", info->title, sizeof info->title);
    clean(info->author);
    clean(info->title);

    int rc = 0;
    doc_rect r;
    if (info->pages > 0 && doc_page_bounds(d, 0, &r, NULL) == 0 && r.x1 > r.x0 && r.y1 > r.y0) {
        float pw = r.x1 - r.x0, ph = r.y1 - r.y0;
        float scale = COVER_W / pw < COVER_H / ph ? COVER_W / pw : COVER_H / ph;
        int w = (int)(pw * scale + 0.5f), h = (int)(ph * scale + 0.5f);
        w = w < 1 ? 1 : w > COVER_W ? COVER_W : w;
        h = h < 1 ? 1 : h > COVER_H ? COVER_H : h;
        uint8_t *img = malloc((size_t)w * h);
        doc_view v = { scale, r.x0, r.y0 };
        int rr = img ? doc_render(d, 0, &v, img, w, h, w, c->cookie, NULL) : -1;
        if (rr == 0) {
            *img_out = img;
            info->img = img;
            info->w = w;
            info->h = h;
        } else {
            free(img);
            rc = rr == 1 ? 1 : 0; /* не нарисовалась — сведения всё равно сохраним */
        }
    }
    doc_close(d);
    return rc;
}

static entry *next_job(covers *c)
{
    entry *best = NULL;
    for (int i = 0; i < c->n; i++)
        if (c->items[i]->state == C_QUEUED && (!best || c->items[i]->seq > best->seq))
            best = c->items[i];
    return best;
}

static void *worker(void *arg)
{
    covers *c = arg;
    pthread_mutex_lock(&c->mu);
    for (;;) {
        entry *e;
        while (!c->quit && (c->paused || !(e = next_job(c))))
            pthread_cond_wait(&c->cv, &c->mu);
        if (c->quit)
            break;
        e->state = C_WORKING;
        doc_cookie_reset(c->cookie);
        char key[17];
        memcpy(key, e->key, sizeof key);
        char *path = strdup(e->path);
        pthread_mutex_unlock(&c->mu);

        char mark[1120];
        mark_path(c, key, mark, sizeof mark);
        int mfd = open(mark, O_WRONLY | O_CREAT, 0644);
        if (mfd >= 0) {
            fsync(mfd); /* запись должна дойти до карты раньше, чем поток уйдёт в разбор книги */
            close(mfd);
        }
        double t0 = clock_ms();
        cover_info info;
        memset(&info, 0, sizeof info);
        uint8_t *img = NULL;
        int rc = path ? render_cover(c, path, &info, &img) : -1;
        remove(mark);
        if (rc == 0)
            save_cached(c, key, &info);
        if (rc != 1)
            log_msg("обложка: %s — %s, %d стр., %.0f ms", path ? path : "?", info.img ? "есть" : "нет", info.pages,
                    clock_ms() - t0);
        free(path);

        pthread_mutex_lock(&c->mu);
        if (rc == 1) {
            e->state = C_QUEUED;
        } else {
            e->info = info;
            e->img = img;
            e->state = rc == 0 ? C_READY : C_FAILED;
            if (img && rc == 0)
                c->nimg++;
            else if (img) {
                free(img);
                e->img = NULL;
                e->info.img = NULL;
            }
            e->used = ++c->use_seq;
            c->ready = 1;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return NULL;
}

/* ---- Снаружи ---- */

covers *covers_new(const char *dir, const doc_options *opt)
{
    covers *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    snprintf(c->dir, sizeof c->dir, "%s", dir);
    mkdir(dir, 0755);
    c->opt = *opt;
    c->cookie = doc_cookie_new();
    pthread_mutex_init(&c->mu, NULL);
    pthread_cond_init(&c->cv, NULL);
    if (!c->cookie || pthread_create(&c->thread, NULL, worker, c) != 0) {
        covers_free(c);
        return NULL;
    }
    c->started = 1;
    return c;
}

void covers_free(covers *c)
{
    if (!c)
        return;
    if (c->started) {
        pthread_mutex_lock(&c->mu);
        c->quit = 1;
        doc_cookie_abort(c->cookie);
        pthread_cond_signal(&c->cv);
        pthread_mutex_unlock(&c->mu);
        /* Вёрстка книги (fz_layout_document) прервать нельзя — не ждём поток дольше пары секунд:
         * процесс всё равно завершается, память потока не освобождаем. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += COVERS_JOIN_S;
        if (pthread_timedjoin_np(c->thread, NULL, &ts) != 0) {
            log_msg("обложки: поток не завершился за %d с — выхожу без ожидания", COVERS_JOIN_S);
            return;
        }
    }
    for (int i = 0; i < c->n; i++) {
        free(c->items[i]->path);
        free(c->items[i]->img);
        free(c->items[i]);
    }
    free(c->items);
    doc_cookie_free(c->cookie);
    pthread_cond_destroy(&c->cv);
    pthread_mutex_destroy(&c->mu);
    free(c);
}

static entry *find_or_add(covers *c, const char *path)
{
    for (int i = 0; i < c->n; i++)
        if (!strcmp(c->items[i]->path, path))
            return c->items[i];
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 32;
        entry **ni = realloc(c->items, sizeof *ni * (size_t)cap);
        if (!ni)
            return NULL;
        c->items = ni;
        c->cap = cap;
    }
    entry *e = calloc(1, sizeof *e);
    if (!e || !(e->path = strdup(path))) {
        free(e);
        return NULL;
    }
    c->items[c->n++] = e;
    return e;
}

/* Обложек в памяти не больше COVERS_IN_MEMORY (330×440 — 145 КБ каждая): самая давно
 * отданная выгружается и при следующем запросе читается с диска заново. Под замком. */
#define COVERS_IN_MEMORY 48

static void evict(covers *c, const entry *keep)
{
    while (c->nimg > COVERS_IN_MEMORY) {
        entry *old = NULL;
        for (int i = 0; i < c->n; i++) {
            entry *e = c->items[i];
            if (e != keep && e->state == C_READY && e->img && (!old || e->used < old->used))
                old = e;
        }
        if (!old)
            return;
        free(old->img);
        old->img = NULL;
        memset(&old->info, 0, sizeof old->info);
        old->state = C_NEW;
        c->nimg--;
    }
}

int covers_get(covers *c, const char *path, const cover_info **out)
{
    pthread_mutex_lock(&c->mu);
    entry *e = find_or_add(c, path);
    int rc = -1;
    if (e && e->state == C_NEW) {
        struct stat st;
        if (!doc_supported(path) || stat(path, &st) != 0) {
            e->state = C_FAILED;
        } else {
            make_key(path, &st, e->key);
            char mark[1120];
            mark_path(c, e->key, mark, sizeof mark);
            int cached = load_cached(c, e) == 0;
            if (!cached && access(mark, F_OK) == 0) {
                log_msg("обложка: %s — прошлая попытка не закончилась, пропускаю", path);
                e->state = C_FAILED;
            } else if (cached) {
                e->state = C_READY;
                c->nimg += e->img != NULL;
            } else {
                free(e->img);
                e->img = NULL;
                memset(&e->info, 0, sizeof e->info);
                e->state = C_QUEUED;
                pthread_cond_signal(&c->cv);
            }
        }
    }
    if (e) {
        switch (e->state) {
        case C_READY:
            e->used = ++c->use_seq;
            evict(c, e);
            *out = &e->info;
            rc = 0;
            break;
        case C_QUEUED:
        case C_WORKING:
            e->seq = ++c->seq;
            rc = 1;
            break;
        default:
            rc = -1;
            break;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return rc;
}

/* Забыть то, что в памяти о книге (готовое и неудавшееся; то, что в работе, не трогаем). Под замком. */
static void drop_entry(covers *c, const char *path)
{
    for (int i = 0; i < c->n; i++) {
        entry *e = c->items[i];
        if (strcmp(e->path, path) || (e->state != C_READY && e->state != C_FAILED))
            continue;
        if (e->img)
            c->nimg--;
        free(e->img);
        e->img = NULL;
        memset(&e->info, 0, sizeof e->info);
        e->state = C_NEW;
    }
}

void covers_move(covers *c, const char *from, const char *to)
{
    struct stat st;
    if (stat(to, &st) != 0)
        return;
    char kf[17], kt[17], a[1120], b[1120];
    make_key(from, &st, kf);
    make_key(to, &st, kt);
    pthread_mutex_lock(&c->mu);
    if (strcmp(kf, kt)) {
        static const char *const ext[] = { "pgm", "inf" };
        for (int i = 0; i < 2; i++) {
            snprintf(a, sizeof a, "%s/%s.%s", c->dir, kf, ext[i]);
            snprintf(b, sizeof b, "%s/%s.%s", c->dir, kt, ext[i]);
            rename(a, b);
        }
        mark_path(c, kf, a, sizeof a);
        remove(a);
    }
    drop_entry(c, from);
    drop_entry(c, to);
    pthread_mutex_unlock(&c->mu);
}

void covers_forget(covers *c, const char *path)
{
    struct stat st;
    pthread_mutex_lock(&c->mu);
    if (stat(path, &st) == 0) {
        char key[17], a[1120];
        make_key(path, &st, key);
        static const char *const ext[] = { "pgm", "inf" };
        for (int i = 0; i < 2; i++) {
            snprintf(a, sizeof a, "%s/%s.%s", c->dir, key, ext[i]);
            remove(a);
        }
        mark_path(c, key, a, sizeof a);
        remove(a);
    }
    drop_entry(c, path);
    pthread_mutex_unlock(&c->mu);
}

int covers_take_ready(covers *c)
{
    pthread_mutex_lock(&c->mu);
    int r = c->ready;
    c->ready = 0;
    pthread_mutex_unlock(&c->mu);
    return r;
}

int covers_pending(covers *c)
{
    pthread_mutex_lock(&c->mu);
    int n = 0;
    for (int i = 0; i < c->n; i++)
        if (c->items[i]->state == C_QUEUED || c->items[i]->state == C_WORKING)
            n++;
    pthread_mutex_unlock(&c->mu);
    return n;
}

void covers_pause(covers *c, int pause)
{
    pthread_mutex_lock(&c->mu);
    c->paused = pause;
    if (pause)
        doc_cookie_abort(c->cookie);
    pthread_cond_signal(&c->cv);
    pthread_mutex_unlock(&c->mu);
}
