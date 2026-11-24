#include "render.h"

#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../base/clock.h"
#include "../base/log.h"

#define MAX_WANTED 16
#define MAX_DONE 16
#define MAX_WORKERS 4

typedef struct {
    struct renderer *r;
    int index;
    doc *doc; /* у каждого потока своя копия: fz_document не потокобезопасен */
    doc_cookie *cookie;
    pthread_t thread;
    int started;
    /* Под r->lock: */
    int current;  /* страница в работе или -1 */
    int aborting; /* текущей уже отправлена отмена */
    int generation; /* r->generation на начало текущей страницы */
} worker;

struct renderer {
    int width, height, stride;

    pthread_mutex_t lock;
    pthread_cond_t cond;

    /* Под lock: */
    int wanted[MAX_WANTED];
    int wanted_count;
    page_image *done[MAX_DONE];
    int done_count;
    int quit;
    int urgent_alone; /* пока срочная не готова, новые страницы не начинать */
    int hold_page;    /* срочная страница, которую ждут остальные, или -1 */
    view_settings view;
    int generation;   /* растёт при смене view: старые отрисовки выбрасываются */

    worker workers[MAX_WORKERS];
    int worker_count;
};

static int index_of(const int *pages, int count, int page)
{
    for (int i = 0; i < count; i++)
        if (pages[i] == page)
            return i;
    return -1;
}

static int is_done(const renderer *r, int page)
{
    for (int i = 0; i < r->done_count; i++)
        if (r->done[i]->page == page)
            return 1;
    return 0;
}

/* Страницу уже рисует поток, который не отменяют. */
static int in_progress(const renderer *r, int page)
{
    for (int i = 0; i < r->worker_count; i++)
        if (r->workers[i].current == page && !r->workers[i].aborting)
            return 1;
    return 0;
}

static void remove_wanted(renderer *r, int page)
{
    int i = index_of(r->wanted, r->wanted_count, page);
    if (i >= 0) {
        memmove(r->wanted + i, r->wanted + i + 1, sizeof(int) * (size_t)(r->wanted_count - i - 1));
        r->wanted_count--;
    }
}

static void abort_worker(worker *w)
{
    if (w->current < 0 || w->aborting)
        return;
    doc_cookie_abort(w->cookie);
    w->aborting = 1;
    log_msg("render[%d]: прерываю стр. %d", w->index, w->current + 1);
}

/* Самая высокая картинка страницы «по ширине», в экранах. */
#define MAX_SCREENS_PER_PAGE 4

/* Как вписать страницу rect (номер page с 0) в экран W×H; *img_h — высота картинки. */
static doc_view plan_view(const view_settings *vs, const doc_rect *rect, int page, int W, int H, int *img_h)
{
    float pw = rect->x1 - rect->x0, ph = rect->y1 - rect->y0;
    *img_h = H;
    if (vs->mode == VIEW_FIT_WIDTH && pw > 0 && ph > 0) {
        float s = W / pw;
        int h = (int)ceilf(ph * s);
        if (h > H) {
            doc_view v = { s, rect->x0, rect->y0 };
            *img_h = h < H * MAX_SCREENS_PER_PAGE ? h : H * MAX_SCREENS_PER_PAGE;
            return v;
        }
    } else if (vs->mode == VIEW_CROP) {
        const doc_rect *c = &vs->crop[page & 1];
        doc_rect cr = { rect->x0 + c->x0 * pw, rect->y0 + c->y0 * ph, rect->x0 + c->x1 * pw,
                        rect->y0 + c->y1 * ph };
        if (cr.x1 > cr.x0 && cr.y1 > cr.y0)
            return doc_fit_page(&cr, W, H);
    }
    return doc_fit_page(rect, W, H);
}

/* Таблица контраста: точка чернее DARK — чёрная, светлее WHITE — белая, между — гамма. */
static void contrast_lut(int level, uint8_t lut[256])
{
    static const struct {
        int black, white;
        float gamma;
    } k[4] = { { 0, 255, 1.0f }, { 0, 235, 1.35f }, { 10, 220, 1.7f }, { 25, 205, 2.2f } };
    if (level < 0 || level > 3)
        level = 0;
    for (int i = 0; i < 256; i++) {
        float v = (float)(i - k[level].black) / (k[level].white - k[level].black);
        v = v < 0 ? 0 : v > 1 ? 1 : v;
        lut[i] = (uint8_t)(255 * powf(v, k[level].gamma) + 0.5f);
    }
}

static void apply_contrast(page_image *img, int level)
{
    if (level <= 0)
        return;
    uint8_t lut[256];
    contrast_lut(level, lut);
    for (int y = 0; y < img->height; y++) {
        uint8_t *row = img->pixels + (size_t)y * img->stride;
        for (int x = 0; x < img->width; x++)
            row[x] = lut[row[x]];
    }
}

#define SPREAD_GUTTER 24 /* просвет между страницами разворота */

/* Страница разворота в половину hw×H: левая прижата к середине справа, правая — слева. */
static doc_view plan_half(const view_settings *vs, const doc_rect *rect, int page, int hw, int H, int right)
{
    view_settings one = *vs;
    if (one.mode == VIEW_FIT_WIDTH)
        one.mode = VIEW_FIT_PAGE; /* в развороте страница всегда целиком */
    int img_h;
    doc_view v = plan_view(&one, rect, page, hw, H, &img_h);
    /* plan_view центрирует; сдвигаем к корешку. */
    float used = 0;
    if (one.mode == VIEW_CROP) {
        const doc_rect *c = &vs->crop[page & 1];
        float pw = rect->x1 - rect->x0;
        used = (c->x1 - c->x0) * pw * v.scale;
    } else {
        used = (rect->x1 - rect->x0) * v.scale;
    }
    float slack = (hw - used) / 2; /* поле с каждой стороны при центрировании */
    if (slack > 0)
        v.x0 += (right ? slack : -slack) / v.scale;
    return v;
}

/* Рисует страницу вне блокировки и отдаёт картинку в *out (NULL — не хватило памяти).
 * 0 — готово, 1 — прервано, -1 — ошибка. */
static int render_page(worker *w, int page, const view_settings *vs, page_image **out)
{
    renderer *r = w->r;
    doc_rect rect;
    doc_timing tb = { 0, 0 }, tr = { 0, 0 };
    double t0 = clock_ms();
    page_image *img = NULL;
    int rc = doc_page_bounds(w->doc, page, &rect, &tb);
    doc_view v = { 1, 0, 0 };
    int img_h = r->height;
    if (rc == 0 && !vs->spread)
        v = plan_view(vs, &rect, page, r->width, r->height, &img_h);
    img = page_image_new(page, r->width, img_h, r->stride);
    *out = img;
    if (!img) {
        log_msg("render[%d]: стр. %d: нет памяти под страницу", w->index, page + 1);
        return -1;
    }
    if (rc == 0 && !vs->spread) {
        img->npages = 1;
        img->lpage[0] = page;
        img->lscale[0] = v.scale, img->lx0[0] = v.x0, img->ly0[0] = v.y0, img->lx[0] = 0;
        rc = doc_render(w->doc, page, &v, img->pixels, img->width, img->height, img->stride, w->cookie, &tr);
    } else if (rc == 0) {
        /* Разворот: page слева, page + 1 справа (если есть). */
        int hw = (r->width - SPREAD_GUTTER) / 2;
        memset(img->pixels, 255, img->bytes);
        img->npages = 0;
        for (int k = 0; k < 2 && rc == 0; k++) {
            int p = page + k;
            if (k == 1 && doc_page_bounds(w->doc, p, &rect, NULL) != 0)
                break; /* правой нет — конец книги */
            doc_view hv = plan_half(vs, &rect, p, hw, r->height, k == 1);
            int x = k ? r->width - hw : 0;
            doc_timing t1 = { 0, 0 };
            rc = doc_render(w->doc, p, &hv, img->pixels + x, hw, img->height, img->stride, w->cookie, &t1);
            tr.render_ms += t1.render_ms;
            img->lpage[k] = p;
            img->lscale[k] = hv.scale, img->lx0[k] = hv.x0, img->ly0[k] = hv.y0, img->lx[k] = x;
            img->npages = k + 1;
        }
    }
    if (rc == 0)
        apply_contrast(img, vs->contrast);
    img->render_ms = clock_ms() - t0;
    if (rc < 0) {
        /* Страница с ошибкой всё равно отдаётся, чтобы её не ждали вечно. */
        log_msg("render[%d]: стр. %d: %s", w->index, img->page + 1, doc_error(w->doc));
        memset(img->pixels, 255, img->bytes);
        img->failed = 1;
    } else if (rc == 1) {
        log_msg("render[%d]: стр. %d прервана через %.1f ms", w->index, img->page + 1, img->render_ms);
    } else {
        log_msg("render[%d]: стр. %d%s за %.1f ms (load %.1f, render %.1f)", w->index, img->page + 1,
                vs->spread ? " (разворот)" : "", img->render_ms, tb.load_ms, tr.render_ms);
    }
    return rc;
}

static void *worker_main(void *arg)
{
    worker *w = arg;
    renderer *r = w->r;
    pthread_mutex_lock(&r->lock);
    for (;;) {
        while (!r->quit && (r->wanted_count == 0 || r->done_count == MAX_DONE ||
                            (r->hold_page >= 0 && r->wanted[0] != r->hold_page)))
            pthread_cond_wait(&r->cond, &r->lock);
        if (r->quit)
            break;

        int page = r->wanted[0];
        remove_wanted(r, page);
        w->current = page;
        w->aborting = 0;
        w->generation = r->generation;
        view_settings vs = r->view;
        doc_cookie_reset(w->cookie);
        pthread_mutex_unlock(&r->lock);

        page_image *img = NULL;
        int rc = render_page(w, page, &vs, &img);

        pthread_mutex_lock(&r->lock);
        w->current = -1;
        w->aborting = 0;
        if (page == r->hold_page && rc != 1) {
            r->hold_page = -1; /* срочная готова — остальные потоки могут начинать */
            pthread_cond_broadcast(&r->cond);
        }
        if (w->generation != r->generation)
            rc = 1; /* нарисована по старому view — не нужна */
        if (img && rc != 1 && r->done_count < MAX_DONE) {
            r->done[r->done_count++] = img;
            /* Отмена могла опоздать: страница готова, а её снова поставили в список. */
            remove_wanted(r, page);
        } else {
            page_image_free(img);
        }
    }
    pthread_mutex_unlock(&r->lock);
    return NULL;
}

renderer *renderer_new(doc **docs, int count, int width, int height, int stride)
{
    renderer *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->width = width;
    r->height = height;
    r->stride = stride;
    r->hold_page = -1;
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);

    for (int i = MAX_WORKERS; i < count; i++)
        doc_close(docs[i]);
    if (count > MAX_WORKERS)
        count = MAX_WORKERS;
    for (int i = 0; i < count; i++) {
        worker *w = &r->workers[i];
        w->r = r;
        w->index = i;
        w->doc = docs[i];
        w->current = -1;
        w->cookie = doc_cookie_new();
        r->worker_count = i + 1;
        if (!w->cookie || pthread_create(&w->thread, NULL, worker_main, w) != 0) {
            log_msg("render: поток %d не запустился", i);
            for (int j = i + 1; j < count; j++)
                doc_close(docs[j]);
            renderer_free(r);
            return NULL;
        }
        w->started = 1;
    }
    return r;
}

void renderer_stop_async(renderer *r)
{
    pthread_mutex_lock(&r->lock);
    r->quit = 1;
    r->wanted_count = 0;
    for (int i = 0; i < r->worker_count; i++)
        abort_worker(&r->workers[i]);
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

void renderer_free(renderer *r)
{
    if (!r)
        return;
    renderer_stop_async(r);
    for (int i = 0; i < r->worker_count; i++) {
        worker *w = &r->workers[i];
        if (w->started)
            pthread_join(w->thread, NULL);
        doc_cookie_free(w->cookie);
        doc_close(w->doc);
    }
    for (int i = 0; i < r->done_count; i++)
        page_image_free(r->done[i]);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r);
}

void renderer_set_urgent_alone(renderer *r, int on)
{
    pthread_mutex_lock(&r->lock);
    r->urgent_alone = on;
    pthread_mutex_unlock(&r->lock);
}

void renderer_request(renderer *r, const int *pages, int count, int urgent)
{
    pthread_mutex_lock(&r->lock);

    /* 1. Страница больше не нужна — прервать. */
    for (int i = 0; i < r->worker_count; i++) {
        worker *w = &r->workers[i];
        if (w->current >= 0 && index_of(pages, count, w->current) < 0)
            abort_worker(w);
    }

    /* 2. Срочную страницу никто не рисует и свободного потока нет —
     *    прервать поток с наименее нужной страницей. */
    if (urgent && count > 0 && !in_progress(r, pages[0])) {
        worker *victim = NULL;
        int victim_pos = -1;
        for (int i = 0; i < r->worker_count; i++) {
            worker *w = &r->workers[i];
            if (w->current < 0 || w->aborting) {
                victim = NULL;
                break; /* свободный поток возьмёт срочную сам */
            }
            int pos = index_of(pages, count, w->current);
            if (pos > victim_pos) {
                victim = w;
                victim_pos = pos;
            }
        }
        if (victim)
            abort_worker(victim);
    }

    /* 3. Срочную рисовать одну: не делить с соседними ядра и блокировку JPX в MuPDF. */
    r->hold_page = urgent && r->urgent_alone && count > 0 && !is_done(r, pages[0]) ? pages[0] : -1;

    /* 4. Новый список: без готовых и уже рисующихся страниц. */
    r->wanted_count = 0;
    for (int i = 0; i < count && r->wanted_count < MAX_WANTED; i++) {
        int p = pages[i];
        if (is_done(r, p) || in_progress(r, p))
            continue;
        r->wanted[r->wanted_count++] = p;
    }
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

int renderer_collect(renderer *r, page_image **out, int max)
{
    pthread_mutex_lock(&r->lock);
    int n = r->done_count < max ? r->done_count : max;
    memcpy(out, r->done, sizeof(page_image *) * (size_t)n);
    memmove(r->done, r->done + n, sizeof(page_image *) * (size_t)(r->done_count - n));
    r->done_count -= n;
    pthread_cond_broadcast(&r->cond); /* место в done освободилось */
    pthread_mutex_unlock(&r->lock);
    return n;
}

int renderer_busy(renderer *r)
{
    pthread_mutex_lock(&r->lock);
    int busy = r->wanted_count > 0 || r->done_count > 0;
    for (int i = 0; i < r->worker_count; i++)
        busy |= r->workers[i].current >= 0;
    pthread_mutex_unlock(&r->lock);
    return busy;
}

void renderer_set_view(renderer *r, const view_settings *v)
{
    pthread_mutex_lock(&r->lock);
    r->view = *v;
    r->generation++;
    for (int i = 0; i < r->worker_count; i++)
        abort_worker(&r->workers[i]);
    for (int i = 0; i < r->done_count; i++)
        page_image_free(r->done[i]);
    r->done_count = 0;
    r->wanted_count = 0;
    r->hold_page = -1;
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}
