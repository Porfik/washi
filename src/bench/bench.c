/* Замер: время открытия документа, загрузки и отрисовки каждой страницы, пик памяти. */
#include "bench.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../base/clock.h"
#include "../doc/doc.h"
#include "../render/render.h"
#include "prof.h"

#define DEFAULT_W 1404
#define DEFAULT_H 1872
#define DEFAULT_STORE_MB 64
#define MAX_PARALLEL 4
#define PARALLEL_WINDOW 16

typedef struct {
    const char *path;
    const char *pages;
    int w, h;
    doc_options doc;
    const char *pgm_dir;
    int repeat;
    int parallel;
    int abort_after_ms;
    const char *compare;
    const char *prof_path;
    int prof_us;
} bench_args;

void bench_usage(FILE *out)
{
    fprintf(out,
            "washi --bench FILE [PAGES] [опции]\n"
            "  PAGES          страницы с 1: \"1-10,15,20-\" (по умолчанию все)\n"
            "  --size WxH     размер вывода и вёрстки EPUB/FB2/HTML, по умолчанию %dx%d\n"
            "  --em N         кегль вёрстки EPUB/FB2/HTML, по умолчанию 28\n"
            "  --store MB     лимит кэша ресурсов MuPDF, по умолчанию %d (0 — без лимита)\n"
            "  --aa BITS      бит сглаживания 0..8 (по умолчанию как в MuPDF)\n"
            "  --no-icc       без ICC-профилей\n"
            "  --no-jpx-reduce  JPX всегда в полном размере\n"
            "  --jpx-threads N  потоков распаковки JPX (по умолчанию 2)\n"
            "  --no-fast      без быстрых путей патча MuPDF 0003 (JPX, 1-битные картинки, маски, цвет, наложение)\n"
            "  --mupdf-upscale  увеличивать картинки, как MuPDF (попиксельно при наложении)\n"
            "  --jpx-min PCT  JPX можно распаковать мельче нужного, до PCT %% (по умолчанию 100)\n"
            "  --jpx-locked   JPX под общей блокировкой MuPDF (без патча 0004)\n"
            "  --repeat N     рисовать каждую страницу N раз подряд\n"
            "  --parallel N   рисовать N потоками, как в читалке (меряется общее время)\n"
            "  --pgm DIR      сохранить страницы в DIR/pNNNN.pgm\n"
            "  --abort-after MS  отменять отрисовку каждой страницы через MS мс: время до остановки,\n"
            "                 затем та же страница целиком и сверка с отрисовкой в новом документе\n"
            "  --slow-abort   отмена только между командами страницы (без патча MuPDF 0006)\n"
            "  --no-neon      без NEON: масштабирование и RGB -> серый (без патча MuPDF 0007)\n"
            "  --flate-check  проверять Adler-32 у данных Flate, как MuPDF (без патча 0007)\n"
            "  --rgb-jpeg     цветной JPEG через RGB, как MuPDF (без патча 0008)\n"
            "  --eager-clip   маска отсечения сразу на всю область, как MuPDF (без патча 0013)\n"
            "  --compare FLAG  каждую страницу рисуют два документа: с опциями как есть и с\n"
            "                 обратным значением --FLAG (no-neon, flate-check, rgb-jpeg, eager-clip, slow-abort,\n"
            "                 no-fast, no-icc, no-jpx-reduce, mupdf-upscale, jpx-locked);\n"
            "                 время и сверка пикселей\n"
            "  --prof FILE    профиль по сэмплам в FILE (scripts/prof-report.py)\n"
            "  --prof-us N    период сэмплов, мкс процессорного времени (по умолчанию 1000)\n",
            DEFAULT_W, DEFAULT_H, DEFAULT_STORE_MB);
}

/* Поле doc_options для опции-флага (имя без "--"), для --compare. */
static int *flag_option(doc_options *o, const char *name)
{
    if (!strcmp(name, "no-icc"))
        return &o->no_icc;
    if (!strcmp(name, "no-jpx-reduce"))
        return &o->jpx_no_reduce;
    if (!strcmp(name, "no-fast"))
        return &o->no_fast_paths;
    if (!strcmp(name, "mupdf-upscale"))
        return &o->mupdf_upscale;
    if (!strcmp(name, "jpx-locked"))
        return &o->jpx_locked;
    if (!strcmp(name, "slow-abort"))
        return &o->slow_abort;
    if (!strcmp(name, "no-neon"))
        return &o->no_neon;
    if (!strcmp(name, "flate-check"))
        return &o->flate_check;
    if (!strcmp(name, "rgb-jpeg"))
        return &o->rgb_jpeg;
    if (!strcmp(name, "eager-clip"))
        return &o->eager_clip;
    return NULL;
}

static int parse_args(int argc, char **argv, bench_args *a)
{
    memset(a, 0, sizeof *a);
    a->w = DEFAULT_W;
    a->h = DEFAULT_H;
    a->doc.store_bytes = (size_t)DEFAULT_STORE_MB << 20;
    a->doc.aa_level = -1;
    a->repeat = 1;
    a->prof_us = 1000;

    for (int i = 0; i < argc; i++) {
        const char *s = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(s, "--em") && v) {
            a->doc.reflow_em = atoi(v);
            i++;
        } else if (!strcmp(s, "--size") && v) {
            if (sscanf(v, "%dx%d", &a->w, &a->h) != 2 || a->w <= 0 || a->h <= 0)
                return -1;
            i++;
        } else if (!strcmp(s, "--store") && v) {
            a->doc.store_bytes = (size_t)atol(v) << 20;
            i++;
        } else if (!strcmp(s, "--aa") && v) {
            a->doc.aa_level = atoi(v);
            i++;
        } else if (!strcmp(s, "--no-icc")) {
            a->doc.no_icc = 1;
        } else if (!strcmp(s, "--no-jpx-reduce")) {
            a->doc.jpx_no_reduce = 1;
        } else if (!strcmp(s, "--no-fast")) {
            a->doc.no_fast_paths = 1;
        } else if (!strcmp(s, "--mupdf-upscale")) {
            a->doc.mupdf_upscale = 1;
        } else if (!strcmp(s, "--jpx-min") && v) {
            a->doc.jpx_min_scale = atoi(v);
            i++;
        } else if (!strcmp(s, "--jpx-locked")) {
            a->doc.jpx_locked = 1;
        } else if (!strcmp(s, "--jpx-threads") && v) {
            a->doc.jpx_threads = atoi(v);
            i++;
        } else if (!strcmp(s, "--repeat") && v) {
            a->repeat = atoi(v) > 0 ? atoi(v) : 1;
            i++;
        } else if (!strcmp(s, "--parallel") && v) {
            a->parallel = atoi(v);
            i++;
        } else if (!strcmp(s, "--abort-after") && v) {
            a->abort_after_ms = atoi(v);
            i++;
        } else if (!strcmp(s, "--slow-abort")) {
            a->doc.slow_abort = 1;
        } else if (!strcmp(s, "--no-neon")) {
            a->doc.no_neon = 1;
        } else if (!strcmp(s, "--flate-check")) {
            a->doc.flate_check = 1;
        } else if (!strcmp(s, "--rgb-jpeg")) {
            a->doc.rgb_jpeg = 1;
        } else if (!strcmp(s, "--eager-clip")) {
            a->doc.eager_clip = 1;
        } else if (!strcmp(s, "--compare") && v) {
            doc_options tmp;
            if (!flag_option(&tmp, v))
                return -1;
            a->compare = v;
            i++;
        } else if (!strcmp(s, "--prof") && v) {
            a->prof_path = v;
            i++;
        } else if (!strcmp(s, "--prof-us") && v) {
            a->prof_us = atoi(v);
            i++;
        } else if (!strcmp(s, "--pgm") && v) {
            a->pgm_dir = v;
            i++;
        } else if (s[0] == '-' && s[1] == '-') {
            return -1;
        } else if (!a->path) {
            a->path = s;
        } else if (!a->pages) {
            a->pages = s;
        } else {
            return -1;
        }
    }
    /* «Перетекающие» книги верстаются в размер вывода: страница в масштабе 1
     * занимает весь буфер целиком. */
    if (a->doc.reflow_w == 0 && a->doc.reflow_h == 0) {
        a->doc.reflow_w = a->w;
        a->doc.reflow_h = a->h;
    }
    return a->path ? 0 : -1;
}

/* Разбирает "1-10,15,20-" в флаги selected[0..count-1]. */
static int parse_pages(const char *spec, int count, unsigned char *selected)
{
    if (!spec) {
        memset(selected, 1, (size_t)count);
        return 0;
    }
    memset(selected, 0, (size_t)count);
    const char *p = spec;
    while (*p) {
        char *end;
        long from = strtol(p, &end, 10);
        if (end == p)
            return -1;
        long to = from;
        p = end;
        if (*p == '-') {
            p++;
            to = strtol(p, &end, 10);
            if (end == p)
                to = count;
            p = end;
        }
        if (from < 1 || to < from)
            return -1;
        for (long i = from; i <= to && i <= count; i++)
            selected[i - 1] = 1;
        if (*p == ',')
            p++;
        else if (*p)
            return -1;
    }
    return 0;
}

static void make_dirs(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    }
    mkdir(buf, 0755);
}

static void write_pgm(const char *dir, int page, const uint8_t *buf, int w, int h, int stride)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/p%04d.pgm", dir, page + 1);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P5\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        fwrite(buf + (size_t)y * stride, 1, (size_t)w, f);
    fclose(f);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static long max_rss_kb(void)
{
    struct rusage ru;
    return getrusage(RUSAGE_SELF, &ru) == 0 ? ru.ru_maxrss : -1;
}

static long rss_kb(void)
{
    long pages = -1;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f) {
        if (fscanf(f, "%*s %ld", &pages) != 1)
            pages = -1;
        fclose(f);
    }
    return pages < 0 ? -1 : pages * (sysconf(_SC_PAGESIZE) / 1024);
}

/* --parallel: страницы рисуют N потоков (render/, по копии документа на поток), как
 * отрисовка заранее в читалке. Забирает first во владение. */
static int bench_parallel(const bench_args *a, doc *first, const unsigned char *selected, int count,
                          FILE *out)
{
    doc *docs[MAX_PARALLEL];
    char err[256];
    int n = a->parallel > MAX_PARALLEL ? MAX_PARALLEL : a->parallel;
    docs[0] = first;
    for (int i = 1; i < n; i++) {
        docs[i] = doc_open(a->path, &a->doc, err, sizeof err);
        if (!docs[i]) {
            fprintf(out, "копия документа %d: %s\n", i, err);
            n = i;
            break;
        }
    }
    renderer *r = renderer_new(docs, n, a->w, a->h, a->w);
    unsigned char *done = calloc((size_t)count + 1, 1);
    if (!r || !done) {
        fprintf(out, "parallel: не удалось запустить потоки\n");
        renderer_free(r);
        free(done);
        return 1;
    }
    int total = 0;
    for (int p = 0; p < count; p++)
        total += selected[p];

    fprintf(out, "%5s %9s\n", "page", "render ms");
    int finished = 0, failed = 0;
    double sum = 0;
    double t0 = clock_ms();
    while (finished < total) {
        int window[PARALLEL_WINDOW], k = 0;
        for (int p = 0; p < count && k < PARALLEL_WINDOW; p++)
            if (selected[p] && !done[p])
                window[k++] = p;
        renderer_request(r, window, k, 0);

        struct timespec ts = { 0, 2 * 1000 * 1000 };
        nanosleep(&ts, NULL);

        page_image *imgs[PARALLEL_WINDOW];
        int m = renderer_collect(r, imgs, PARALLEL_WINDOW);
        for (int i = 0; i < m; i++) {
            if (!done[imgs[i]->page]) {
                done[imgs[i]->page] = 1;
                finished++;
                failed += imgs[i]->failed;
                sum += imgs[i]->render_ms;
                fprintf(out, "%5d %9.1f%s\n", imgs[i]->page + 1, imgs[i]->render_ms,
                        imgs[i]->failed ? " ошибка" : "");
            }
            page_image_free(imgs[i]);
        }
    }
    double wall = clock_ms() - t0;
    size_t heap_peak;
    doc_mem_stats(NULL, &heap_peak);
    fprintf(out,
            "summary: parallel %d: pages %d, failed %d; wall %.0f ms = %.1f ms/page; "
            "sum of renders %.0f ms = %.1f ms/page\n",
            n, total, failed, wall, wall / total, sum, sum / total);
    fprintf(out, "memory: mupdf heap peak %.1f MB, RSS %.1f MB (max за процесс %.1f MB)\n",
            heap_peak / 1048576.0, rss_kb() / 1024.0, max_rss_kb() / 1024.0);
    renderer_free(r);
    free(done);
    return failed ? 1 : 0;
}

/* ---- --abort-after: отмена посреди страницы ---- */

typedef struct {
    doc_cookie *cookie;
    int after_ms;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int finished;    /* отрисовка уже вернулась */
    double fired_ms; /* когда отправлена отмена; 0 — не понадобилась */
    const char *prof_path;
    int prof_us;
} abort_timer;

static void *abort_timer_main(void *arg)
{
    abort_timer *t = arg;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += t->after_ms / 1000;
    until.tv_nsec += (long)(t->after_ms % 1000) * 1000000L;
    if (until.tv_nsec >= 1000000000L) {
        until.tv_sec++;
        until.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&t->lock);
    int rc = 0;
    while (!t->finished && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&t->cond, &t->lock, &until);
    if (!t->finished) {
        t->fired_ms = clock_ms();
        doc_cookie_abort(t->cookie);
        /* --prof с --abort-after: профиль только от отмены до остановки — что не прерывается. */
        if (t->prof_path)
            prof_start(t->prof_us);
    }
    pthread_mutex_unlock(&t->lock);
    return NULL;
}

static uint64_t hash_pixels(const uint8_t *buf, int w, int h, int stride)
{
    uint64_t x = 0xcbf29ce484222325ull; /* FNV-1a */
    for (int y = 0; y < h; y++) {
        const uint8_t *p = buf + (size_t)y * stride;
        for (int i = 0; i < w; i++)
            x = (x ^ p[i]) * 0x100000001b3ull;
    }
    return x;
}

/* Отрисовка с отменой через a->abort_after_ms; затем страница целиком в том же документе
 * (ничего недорисованного не должно остаться в кэше MuPDF) и в только что открытом. */
static int bench_abort(const bench_args *a, doc *d, const unsigned char *selected, int count, uint8_t *buf,
                       FILE *out)
{
    char err[256];
    doc *fresh = doc_open(a->path, &a->doc, err, sizeof err);
    doc_cookie *cookie = doc_cookie_new();
    if (!fresh || !cookie) {
        fprintf(out, "abort: %s\n", fresh ? "нет памяти" : err);
        doc_close(fresh);
        doc_cookie_free(cookie);
        return 1;
    }
    fprintf(out, "%5s %9s %14s %9s  %s\n", "page", "отмена ms", "остановка ms", "всего ms", "повтор");
    int pages = 0, stopped = 0, bad = 0;
    double lat_sum = 0, lat_max = 0;
    for (int p = 0; p < count; p++) {
        if (!selected[p])
            continue;
        doc_rect rect;
        if (doc_page_bounds(d, p, &rect, NULL) < 0) {
            fprintf(out, "%5d ошибка: %s\n", p + 1, doc_error(d));
            bad++;
            continue;
        }
        doc_view v = doc_fit_page(&rect, a->w, a->h);

        abort_timer t;
        memset(&t, 0, sizeof t);
        t.cookie = cookie;
        t.after_ms = a->abort_after_ms;
        t.prof_path = a->prof_path;
        t.prof_us = a->prof_us;
        pthread_mutex_init(&t.lock, NULL);
        pthread_cond_init(&t.cond, NULL);
        doc_cookie_reset(cookie);
        pthread_t th;
        double t0 = clock_ms();
        int started = pthread_create(&th, NULL, abort_timer_main, &t) == 0;
        int rc = doc_render(d, p, &v, buf, a->w, a->h, a->w, cookie, NULL);
        double t1 = clock_ms();
        pthread_mutex_lock(&t.lock);
        t.finished = 1;
        pthread_cond_signal(&t.cond);
        pthread_mutex_unlock(&t.lock);
        if (started)
            pthread_join(th, NULL);
        if (t.prof_path && t.fired_ms > 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s.p%d", t.prof_path, p + 1);
            prof_stop(path);
        }
        pthread_mutex_destroy(&t.lock);
        pthread_cond_destroy(&t.cond);

        uint64_t h_same = 0, h_fresh = 0;
        int rc_same = doc_render(d, p, &v, buf, a->w, a->h, a->w, NULL, NULL);
        if (rc_same == 0)
            h_same = hash_pixels(buf, a->w, a->h, a->w);
        doc_rect r0;
        int rc_fresh = doc_page_bounds(fresh, p, &r0, NULL) == 0
                           ? doc_render(fresh, p, &v, buf, a->w, a->h, a->w, NULL, NULL)
                           : -1;
        if (rc_fresh == 0)
            h_fresh = hash_pixels(buf, a->w, a->h, a->w);
        int same = rc_same == 0 && rc_fresh == 0 && h_same == h_fresh;
        bad += !same;
        pages++;

        char stop[32];
        if (rc == 1 && t.fired_ms > 0) {
            double lat = t1 - t.fired_ms;
            lat_sum += lat;
            lat_max = lat > lat_max ? lat : lat_max;
            stopped++;
            snprintf(stop, sizeof stop, "%.1f", lat);
        } else {
            snprintf(stop, sizeof stop, rc == 0 ? "успела" : rc == 1 ? "?" : "ошибка");
        }
        fprintf(out, "%5d %9d %14s %9.1f  %s\n", p + 1, a->abort_after_ms, stop, t1 - t0,
                same ? "совпадает" : "НЕ СОВПАДАЕТ");
        fflush(out);
    }
    fprintf(out, "summary: abort: pages %d, stopped %d, stop avg %.1f, max %.1f ms; mismatched %d\n", pages,
            stopped, stopped ? lat_sum / stopped : 0, lat_max, bad);
    doc_cookie_free(cookie);
    doc_close(fresh);
    return bad ? 1 : 0;
}

/* --compare: каждую страницу рисуют два документа — с опциями a->doc (A) и с обратным
 * значением флага (B); у каждого свой кэш MuPDF, так что оба распаковывают картинки сами. */
static int bench_compare(const bench_args *a, doc *d, const unsigned char *selected, int count, uint8_t *buf,
                         FILE *out)
{
    char err[256];
    doc_options ob = a->doc;
    int *flag = flag_option(&ob, a->compare);
    *flag = !*flag;
    doc *other = doc_open(a->path, &ob, err, sizeof err);
    if (!other) {
        fprintf(out, "compare: %s\n", err);
        return 1;
    }
    uint8_t *buf_b = malloc((size_t)a->w * a->h);
    if (!buf_b) {
        doc_close(other);
        return 1;
    }
    fprintf(out, "compare: A — как задано, B — --%s %s\n", a->compare, *flag ? "включено" : "выключено");
    fprintf(out, "%5s %9s %9s  %s\n", "page", "A ms", "B ms", "пиксели (разных, наибольшая и средняя разница)");
    int pages = 0, bad = 0, max_all = 0;
    double sum_a = 0, sum_b = 0, diff_share = 0;
    for (int p = 0; p < count; p++) {
        if (!selected[p])
            continue;
        doc *docs[2] = { d, other };
        uint8_t *bufs[2] = { buf, buf_b };
        double ms[2];
        int rc[2];
        for (int k = 0; k < 2; k++) {
            doc_rect rect;
            doc_timing tr = { 0, 0 };
            doc_apply_switches(docs[k]);
            rc[k] = doc_page_bounds(docs[k], p, &rect, NULL);
            if (rc[k] == 0) {
                doc_view v = doc_fit_page(&rect, a->w, a->h);
                rc[k] = doc_render(docs[k], p, &v, bufs[k], a->w, a->h, a->w, NULL, &tr);
            }
            ms[k] = tr.render_ms;
        }
        size_t n = (size_t)a->w * a->h, differ = 0;
        long long sum_abs = 0;
        int max_diff = 0;
        for (size_t i = 0; i < n; i++) {
            int dlt = buf[i] - buf_b[i];
            if (dlt) {
                dlt = dlt < 0 ? -dlt : dlt;
                differ++;
                sum_abs += dlt;
                max_diff = dlt > max_diff ? dlt : max_diff;
            }
        }
        int same = rc[0] == 0 && rc[1] == 0 && differ == 0;
        bad += !same;
        pages++;
        sum_a += ms[0];
        sum_b += ms[1];
        max_all = max_diff > max_all ? max_diff : max_all;
        diff_share += (double)differ / n;
        if (rc[0] || rc[1])
            fprintf(out, "%5d %9.1f %9.1f  ошибка\n", p + 1, ms[0], ms[1]);
        else if (same)
            fprintf(out, "%5d %9.1f %9.1f  совпадают\n", p + 1, ms[0], ms[1]);
        else
            fprintf(out, "%5d %9.1f %9.1f  РАЗНЫЕ: %.2f%%, макс %d, сред %.3f\n", p + 1, ms[0], ms[1],
                    100.0 * differ / n, max_diff, (double)sum_abs / n);
        fflush(out);
    }
    if (pages)
        fprintf(out,
                "summary: compare --%s: pages %d, A avg %.1f ms, B avg %.1f ms, B/A %.3f; mismatched %d "
                "(pixels %.2f%%, max diff %d)\n",
                a->compare, pages, sum_a / pages, sum_b / pages, sum_a > 0 ? sum_b / sum_a : 0, bad,
                100.0 * diff_share / pages, max_all);
    free(buf_b);
    doc_close(other);
    doc_apply_switches(d);
    return bad ? 1 : 0;
}

static int bench_file(bench_args a, FILE *out)
{
    struct stat st;
    double file_mb = stat(a.path, &st) == 0 ? st.st_size / 1048576.0 : -1;
    fprintf(out, "file: %s (%.1f MB)\n", a.path, file_mb);
    fprintf(out, "output: %dx%d gray8, store %zu MB, aa %d, icc %s, jpx reduce %s threads %d, fast paths %s, "
            "upscale %s, jpx min %d%%%s, abort %s, neon %s, flate check %s, jpeg %s, repeat %d, parallel %d\n",
            a.w, a.h, a.doc.store_bytes >> 20, a.doc.aa_level, a.doc.no_icc ? "off" : "on",
            a.doc.jpx_no_reduce ? "off" : "on", a.doc.jpx_threads > 0 ? a.doc.jpx_threads : 2,
            a.doc.no_fast_paths ? "off" : "on", a.doc.mupdf_upscale ? "mupdf" : "scaler",
            a.doc.jpx_min_scale > 0 && a.doc.jpx_min_scale < 100 ? a.doc.jpx_min_scale : 100,
            a.doc.jpx_locked ? " locked" : "", a.doc.slow_abort ? "slow" : "fast", a.doc.no_neon ? "off" : "on", a.doc.flate_check ? "on" : "off",
            a.doc.no_icc && !a.doc.rgb_jpeg ? "gray" : "rgb", a.repeat,
            a.parallel > 1 ? a.parallel : 1);

    char err[256];
    doc_mem_reset_peak();
    double t0 = clock_ms();
    doc *d = doc_open(a.path, &a.doc, err, sizeof err);
    double open_ms = clock_ms() - t0;
    if (!d) {
        fprintf(out, "open: ошибка: %s\n", err);
        return 1;
    }
    int count = doc_page_count(d);
    fprintf(out, "open: %.1f ms, pages: %d\n", open_ms, count);

    unsigned char *selected = malloc((size_t)count + 1);
    double *render_times = malloc(sizeof(double) * ((size_t)count * a.repeat + 1));
    int stride = a.w; /* в читалке шаг берётся из буфера экрана */
    uint8_t *buf = malloc((size_t)stride * a.h);
    if (!selected || !render_times || !buf || parse_pages(a.pages, count, selected) < 0) {
        fprintf(out, "ошибка: неверные страницы или нет памяти\n");
        doc_close(d);
        return 2;
    }
    if (a.pgm_dir)
        make_dirs(a.pgm_dir);

    if (a.parallel > 1) {
        int rc = bench_parallel(&a, d, selected, count, out);
        free(buf);
        free(render_times);
        free(selected);
        return rc;
    }
    if (a.abort_after_ms > 0 || a.compare) {
        int rc = a.compare ? bench_compare(&a, d, selected, count, buf, out)
                           : bench_abort(&a, d, selected, count, buf, out);
        free(buf);
        free(render_times);
        free(selected);
        doc_close(d);
        return rc;
    }

    fprintf(out, "%5s %11s %8s %9s %5s %8s\n", "page", "size pt", "load ms", "render ms", "warn",
            "heap MB");
    int n = 0, failed = 0;
    double load_total = 0;
    double t_pages = clock_ms();
    for (int p = 0; p < count; p++) {
        if (!selected[p])
            continue;
        for (int r = 0; r < a.repeat; r++) {
            doc_timing tb = { 0, 0 }, tr = { 0, 0 };
            doc_rect rect;
            int warn0 = doc_warning_count(d);
            if (doc_page_bounds(d, p, &rect, &tb) < 0) {
                fprintf(out, "%5d ошибка: %s\n", p + 1, doc_error(d));
                failed++;
                break;
            }
            doc_view v = doc_fit_page(&rect, a.w, a.h);
            if (doc_render(d, p, &v, buf, a.w, a.h, stride, NULL, &tr) < 0) {
                fprintf(out, "%5d ошибка: %s\n", p + 1, doc_error(d));
                failed++;
                break;
            }
            size_t heap;
            doc_mem_stats(&heap, NULL);
            fprintf(out, "%5d %5.0fx%-5.0f %8.1f %9.1f %5d %8.1f\n", p + 1, rect.x1 - rect.x0,
                    rect.y1 - rect.y0, tb.load_ms, tr.render_ms, doc_warning_count(d) - warn0,
                    heap / 1048576.0);
            fflush(out);
            load_total += tb.load_ms;
            render_times[n++] = tr.render_ms;
            if (a.pgm_dir && r == 0)
                write_pgm(a.pgm_dir, p, buf, a.w, a.h, stride);
        }
    }
    double pages_ms = clock_ms() - t_pages;

    size_t heap_peak;
    doc_mem_stats(NULL, &heap_peak);
    if (n > 0) {
        double sum = 0;
        for (int i = 0; i < n; i++)
            sum += render_times[i];
        qsort(render_times, (size_t)n, sizeof *render_times, cmp_double);
        fprintf(out,
                "summary: renders %d, failed %d; render avg %.1f, median %.1f, p90 %.1f, max %.1f ms; "
                "load avg %.1f ms; wall %.0f ms\n",
                n, failed, sum / n, render_times[n / 2], render_times[(int)(n * 0.9)],
                render_times[n - 1], load_total / n, pages_ms);
    }
    fprintf(out, "memory: mupdf heap peak %.1f MB, RSS %.1f MB (max за процесс %.1f MB)\n",
            heap_peak / 1048576.0, rss_kb() / 1024.0, max_rss_kb() / 1024.0);

    free(buf);
    free(render_times);
    free(selected);
    doc_close(d);
    return failed ? 1 : 0;
}

int bench_main(int argc, char **argv, FILE *out)
{
    bench_args a;
    if (parse_args(argc, argv, &a) < 0) {
        bench_usage(stderr);
        return 2;
    }
    int whole = a.prof_path && a.abort_after_ms <= 0;
    if (whole && prof_start(a.prof_us) < 0)
        fprintf(out, "prof: не запустился\n");
    int rc = bench_file(a, out);
    if (whole)
        fprintf(out, "prof: %d сэмплов -> %s\n", prof_stop(a.prof_path), a.prof_path);
    return rc;
}
