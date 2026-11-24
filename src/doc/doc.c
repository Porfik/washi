#include "doc.h"

#include <mupdf/fitz.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../base/clock.h"
#include "reflow_check.h"

struct doc {
    fz_context *ctx;
    fz_document *fz;
    int page_count;

    fz_page *page; /* последняя загруженная страница */
    int page_no;
    fz_rect page_bounds;

    int warnings;
    int reflowable; /* EPUB/FB2/HTML: свёрстан под экран при открытии */
    float reflow_w, reflow_h, reflow_em;
    doc_options opt;
    char err[256];
};

/* ---- Учёт памяти MuPDF ---- */

typedef union {
    max_align_t align;
    size_t size;
} mem_header;

static atomic_size_t mem_current;
static atomic_size_t mem_peak;

static void mem_grow(size_t n)
{
    size_t cur = atomic_fetch_add(&mem_current, n) + n;
    size_t peak = atomic_load(&mem_peak);
    while (cur > peak && !atomic_compare_exchange_weak(&mem_peak, &peak, cur))
        ;
}

static void *mem_malloc(void *user, size_t size)
{
    (void)user;
    mem_header *h = malloc(sizeof *h + size);
    if (!h)
        return NULL;
    h->size = size;
    mem_grow(size);
    return h + 1;
}

static void mem_free(void *user, void *p)
{
    (void)user;
    if (!p)
        return;
    mem_header *h = (mem_header *)p - 1;
    atomic_fetch_sub(&mem_current, h->size);
    free(h);
}

static void *mem_realloc(void *user, void *p, size_t size)
{
    if (!p)
        return mem_malloc(user, size);
    if (size == 0) {
        mem_free(user, p);
        return NULL;
    }
    mem_header *h = (mem_header *)p - 1;
    size_t old = h->size;
    mem_header *n = realloc(h, sizeof *n + size);
    if (!n)
        return NULL;
    n->size = size;
    atomic_fetch_sub(&mem_current, old);
    mem_grow(size);
    return n + 1;
}

static const fz_alloc_context mem_alloc = { NULL, mem_malloc, mem_realloc, mem_free };

void doc_mem_stats(size_t *current, size_t *peak)
{
    if (current)
        *current = atomic_load(&mem_current);
    if (peak)
        *peak = atomic_load(&mem_peak);
}

void doc_mem_reset_peak(void)
{
    atomic_store(&mem_peak, atomic_load(&mem_current));
}

/* ---- Блокировки MuPDF: общие для всех контекстов, хватает для клонов fz_clone_context ---- */

static pthread_mutex_t fz_mutexes[FZ_LOCK_MAX] = {
    PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER
};

static void fz_lock_fn(void *user, int lock)
{
    (void)user;
    pthread_mutex_lock(&fz_mutexes[lock]);
}

static void fz_unlock_fn(void *user, int lock)
{
    (void)user;
    pthread_mutex_unlock(&fz_mutexes[lock]);
}

static const fz_locks_context fz_locks = { NULL, fz_lock_fn, fz_unlock_fn };

_Static_assert(FZ_LOCK_MAX == 3, "fz_mutexes: число блокировок MuPDF изменилось");

/* ---- Отмена ---- */

struct doc_cookie {
    fz_cookie fz;
};

doc_cookie *doc_cookie_new(void)
{
    return calloc(1, sizeof(doc_cookie));
}

void doc_cookie_free(doc_cookie *c)
{
    free(c);
}

void doc_cookie_reset(doc_cookie *c)
{
    memset(&c->fz, 0, sizeof c->fz);
}

void doc_cookie_abort(doc_cookie *c)
{
    /* MuPDF читает поле abort без синхронизации — так задумано в его API;
     * с нашей стороны запись и чтение атомарные. */
    __atomic_store_n(&c->fz.abort, 1, __ATOMIC_RELAXED);
}

/* Переключатели патчей patches/mupdf/0002–0004. Общие для всех документов. */
extern int fz_washi_jpx_reduce;
extern int fz_washi_jpx_threads;
extern int fz_washi_jpx_fast_copy;
extern int fz_washi_mono_subsample;
extern int fz_washi_mask_clip;
extern int fz_washi_lut_convert;
extern int fz_washi_paint_rows;
extern int fz_washi_jpx_min_scale;
extern const fz_alloc_context *fz_washi_opj_alloc;
extern int fz_washi_neon;
extern int fz_washi_flate_check;
extern int fz_washi_gray_jpeg;
extern int fz_washi_lazy_clip;
/* Патч 0006: флаг отмены этого потока, его видят распаковщики картинок. */
extern _Thread_local const int *fz_washi_abort;

/* ---- Документ ---- */

doc_view doc_fit_page(const doc_rect *r, int w, int h)
{
    float pw = r->x1 - r->x0, ph = r->y1 - r->y0;
    float sx = w / pw, sy = h / ph;
    doc_view v;
    v.scale = sx < sy ? sx : sy;
    v.x0 = r->x0 - (w / v.scale - pw) / 2;
    v.y0 = r->y0 - (h / v.scale - ph) / 2;
    return v;
}

static void on_warning(void *user, const char *message)
{
    (void)message;
    ((doc *)user)->warnings++;
}

static void set_error(doc *d, const char *what, const char *message)
{
    snprintf(d->err, sizeof d->err, "%s: %s", what, message ? message : "?");
}

/* Стили для «перетекающих» книг (fz_set_user_css, не-PDF): чёрный текст, выравнивание
 * по ширине, переносы (если собрано с FZ_ENABLE_HYPHEN), поля в em — масштабируются с кеглем. */
static const char *REFLOW_CSS =
    "body { color: black; margin: 1.1em 0.9em; line-height: 1.35; text-align: justify; "
    "hyphens: auto; }\n"
    "p { margin: 0.35em 0; text-indent: 1.2em; }\n"
    "h1,h2,h3,h4,h5,h6 { text-align: left; margin: 0.9em 0 0.4em 0; font-weight: bold; }\n"
    "img { max-width: 100%; }\n"
    "a { color: black; text-decoration: none; }\n"
    "pre { white-space: pre-wrap; overflow-wrap: break-word; font-size: 0.85em; }\n"
    "table { border-collapse: collapse; }\n"
    "td, th { padding: 2px 5px; border: 1px solid black; }\n";

/* Скаляр по расширению: PDF вёрстке не поддаётся и стили не нужны —
 * чтобы PDF-путь остался прежним, CSS ставим только для остальных форматов. */
static int is_pdf_ext(const char *path)
{
    size_t n = strlen(path);
    return n > 4 && !strcasecmp(path + n - 4, ".pdf");
}

static int open_document(doc *const d, const char *path, char *err, size_t err_len)
{
    /* Перетекающие книги: до MuPDF отсеять файлы, на которых разбор/вёрстка не кончатся
     * (патологическая вложенность, огромный текст) — прервать их изнутри нельзя. */
    if (!is_pdf_ext(path) && reflow_check(path, err, err_len) != 0) {
        return -1; /* причина уже в err */
    }
    fz_try(d->ctx)
    {
        fz_register_document_handlers(d->ctx);
        /* Стили до открытия: HTML-движок применяет их при разметке документа. */
        if (!is_pdf_ext(path))
            fz_set_user_css(d->ctx, REFLOW_CSS);
        d->fz = fz_open_document(d->ctx, path);
        if (fz_needs_password(d->ctx, d->fz))
            fz_throw(d->ctx, FZ_ERROR_GENERIC, "документ защищён паролем");
        /* Перетекающие книги (EPUB/FB2/HTML): вёрстка под заданный размер до
         * подсчёта страниц — число страниц определяется вёрсткой. */
        if (fz_is_document_reflowable(d->ctx, d->fz)) {
            float w = d->opt.reflow_w > 0 ? (float)d->opt.reflow_w : 595;
            float h = d->opt.reflow_h > 0 ? (float)d->opt.reflow_h : 842;
            float em = d->opt.reflow_em > 0 ? (float)d->opt.reflow_em : 28;
            fz_layout_document(d->ctx, d->fz, w, h, em);
            d->reflowable = 1;
            d->reflow_w = w;
            d->reflow_h = h;
            d->reflow_em = em;
        }
        d->page_count = fz_count_pages(d->ctx, d->fz);
    }
    fz_catch(d->ctx)
    {
        snprintf(err, err_len, "%s", fz_caught_message(d->ctx));
        return -1;
    }
    return 0;
}

/* Какие картинки масштабировать заранее (fz_scale_pixmap), а не на лету при наложении.
 * MuPDF по умолчанию — только уменьшаемые; на лету увеличение идёт билинейно по пикселю
 * (template_affine_N_lerp), на B288 это медленно. Масштабатор раздельный и на ARM написан
 * на ассемблере, поэтому ему же отдаём и увеличение до 2 раз. Больше 2 раз MuPDF
 * намеренно не сглаживает картинки без /Interpolate — это оставляем как есть. */
static int scale_up_to_2x(void *arg, int dst_w, int dst_h, int src_w, int src_h)
{
    (void)arg;
    if (dst_w < src_w && dst_h < src_h)
        return 1;
    if (dst_w == src_w && dst_h == src_h)
        return 0;
    return dst_w <= 2 * src_w && dst_h <= 2 * src_h;
}

/* Переключатели патчей MuPDF общие для процесса: ставятся при открытии документа. */
static void apply_switches(const doc_options *opt)
{
    fz_washi_jpx_reduce = !opt->jpx_no_reduce;
    fz_washi_jpx_threads = opt->jpx_threads > 0 ? opt->jpx_threads : 2;
    int fast = !opt->no_fast_paths;
    fz_washi_jpx_fast_copy = fz_washi_mono_subsample = fz_washi_mask_clip = fast;
    fz_washi_lut_convert = fz_washi_paint_rows = fast;
    fz_washi_lazy_clip = fast && !opt->eager_clip; /* патч 0013 */
    fz_washi_jpx_min_scale = opt->jpx_min_scale > 0 && opt->jpx_min_scale < 100 ? opt->jpx_min_scale : 100;
    /* Наш распределитель потокобезопасен, поэтому OpenJPEG может брать память прямо у него. */
    fz_washi_opj_alloc = opt->jpx_locked ? NULL : &mem_alloc;
    fz_washi_neon = !opt->no_neon;
    fz_washi_flate_check = opt->flate_check;
    /* Серый из яркости JPEG — только без ICC: с профилями цвет считает lcms. */
    fz_washi_gray_jpeg = opt->no_icc && !opt->rgb_jpeg;
}

int doc_supported(const char *path)
{
    static const char *exts[] = { ".pdf", ".epub", ".fb2", ".html", ".htm", ".xhtml" };
    size_t n = strlen(path);
    for (int k = 0; k < (int)(sizeof exts / sizeof exts[0]); k++) {
        size_t l = strlen(exts[k]);
        if (n > l && !strcasecmp(path + n - l, exts[k]))
            return 1;
    }
    return 0;
}

int doc_reflowable(const doc *d)
{
    return d->reflowable;
}

doc *doc_open(const char *path, const doc_options *opt, char *err, size_t err_len)
{
    doc *d = calloc(1, sizeof *d);
    if (!d) {
        snprintf(err, err_len, "нет памяти");
        return NULL;
    }
    d->page_no = -1;

    d->ctx = fz_new_context(&mem_alloc, &fz_locks, opt->store_bytes);
    if (!d->ctx) {
        snprintf(err, err_len, "fz_new_context не удался");
        free(d);
        return NULL;
    }
    fz_set_warning_callback(d->ctx, on_warning, d);
    if (opt->aa_level >= 0)
        fz_set_aa_level(d->ctx, opt->aa_level);
    if (opt->no_icc)
        fz_disable_icc(d->ctx);
    if (!opt->mupdf_upscale)
        fz_tune_image_scale(d->ctx, scale_up_to_2x, NULL);
    d->opt = *opt;
    apply_switches(opt);

    if (open_document(d, path, err, err_len) < 0) {
        doc_close(d);
        return NULL;
    }
    return d;
}

void doc_apply_switches(const doc *d)
{
    apply_switches(&d->opt);
}

void doc_close(doc *d)
{
    if (!d)
        return;
    fz_drop_page(d->ctx, d->page);
    fz_drop_document(d->ctx, d->fz);
    fz_drop_context(d->ctx);
    free(d);
}

int doc_page_count(const doc *d)
{
    return d->page_count;
}

#define OUTLINE_MAX 4000

typedef struct {
    doc_outline_item *items;
    int n, cap;
} outline_list;

static int page_of(doc *d, fz_location loc)
{
    int page = -1;
    fz_var(page);
    fz_try(d->ctx) page = fz_page_number_from_location(d->ctx, d->fz, loc);
    fz_catch(d->ctx) page = -1;
    return page < d->page_count ? page : -1;
}

static void flatten(doc *d, fz_outline *o, int level, outline_list *l)
{
    for (; o && l->n < OUTLINE_MAX; o = o->next) {
        if (l->n == l->cap) {
            int cap = l->cap ? l->cap * 2 : 64;
            doc_outline_item *ni = realloc(l->items, sizeof *ni * (size_t)cap);
            if (!ni)
                return;
            l->items = ni;
            l->cap = cap;
        }
        doc_outline_item *it = &l->items[l->n++];
        snprintf(it->title, sizeof it->title, "%s", o->title ? o->title : "");
        /* Пробелы и переводы строк внутри заголовка — одним пробелом. */
        char *w = it->title;
        int space = 1;
        for (const char *p = it->title; *p; p++) {
            int sp = *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
            if (sp && space)
                continue;
            *w++ = sp ? ' ' : *p;
            space = sp;
        }
        while (w > it->title && w[-1] == ' ')
            w--;
        *w = 0;
        it->level = level;
        it->page = page_of(d, o->page);
        if (o->down)
            flatten(d, o->down, level + 1, l);
    }
}

int doc_outline(doc *d, doc_outline_item **out)
{
    *out = NULL;
    fz_outline *root = NULL;
    fz_var(root);
    fz_try(d->ctx) root = fz_load_outline(d->ctx, d->fz);
    fz_catch(d->ctx) root = NULL;
    if (!root)
        return 0;
    outline_list l = { NULL, 0, 0 };
    flatten(d, root, 0, &l);
    fz_drop_outline(d->ctx, root);
    *out = l.items;
    return l.n;
}

static int load_page(doc *d, int page_no, doc_timing *t);

static int link_target(doc *d, const char *uri)
{
    int page = -1;
    fz_var(page);
    fz_try(d->ctx)
    {
        if (uri && !fz_is_external_link(d->ctx, uri)) {
            float x, y;
            fz_location loc = fz_resolve_link(d->ctx, d->fz, uri, &x, &y);
            page = fz_page_number_from_location(d->ctx, d->fz, loc);
        }
    }
    fz_catch(d->ctx) page = -1;
    return page >= 0 && page < d->page_count ? page : -1;
}

int doc_links(doc *d, int page, doc_link **out)
{
    *out = NULL;
    if (load_page(d, page, NULL) != 0)
        return 0;
    fz_link *links = NULL;
    fz_var(links);
    fz_try(d->ctx) links = fz_load_links(d->ctx, d->page);
    fz_catch(d->ctx) links = NULL;
    int n = 0, cap = 0;
    doc_link *items = NULL;
    for (fz_link *l = links; l; l = l->next) {
        int target = link_target(d, l->uri);
        if (target < 0)
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            doc_link *ni = realloc(items, sizeof *ni * (size_t)cap);
            if (!ni)
                break;
            items = ni;
        }
        items[n].rect = (doc_rect){ l->rect.x0, l->rect.y0, l->rect.x1, l->rect.y1 };
        items[n].page = target;
        n++;
    }
    fz_drop_link(d->ctx, links);
    *out = items;
    return n;
}

int doc_meta(doc *d, const char *key, char *buf, size_t len)
{
    if (!len)
        return -1;
    buf[0] = 0;
    int n = -1;
    fz_var(n);
    fz_try(d->ctx) n = fz_lookup_metadata(d->ctx, d->fz, key, buf, len);
    fz_catch(d->ctx) n = -1;
    if (n <= 0) {
        buf[0] = 0;
        return -1;
    }
    char *s = buf;
    while (*s == ' ' || *s == '\t')
        s++;
    memmove(buf, s, strlen(s) + 1);
    size_t l = strlen(buf);
    while (l > 0 && (buf[l - 1] == ' ' || buf[l - 1] == '\t' || buf[l - 1] == '\r' || buf[l - 1] == '\n'))
        buf[--l] = 0;
    return l ? 0 : -1;
}

static int load_page(doc *d, int page_no, doc_timing *t)
{
    double t0 = clock_ms();
    if (d->page_no == page_no) {
        if (t)
            t->load_ms = 0;
        return 0;
    }
    if (page_no < 0 || page_no >= d->page_count) {
        snprintf(d->err, sizeof d->err, "нет страницы %d", page_no + 1);
        return -1;
    }

    fz_drop_page(d->ctx, d->page);
    d->page = NULL;
    d->page_no = -1;

    fz_try(d->ctx)
    {
        d->page = fz_load_page(d->ctx, d->fz, page_no);
        d->page_bounds = fz_bound_page(d->ctx, d->page);
        d->page_no = page_no;
    }
    fz_catch(d->ctx)
    {
        set_error(d, "загрузка страницы", fz_caught_message(d->ctx));
        return -1;
    }
    if (t)
        t->load_ms = clock_ms() - t0;
    return 0;
}

int doc_page_bounds(doc *d, int page_no, doc_rect *out, doc_timing *t)
{
    if (load_page(d, page_no, t) < 0)
        return -1;
    out->x0 = d->page_bounds.x0;
    out->y0 = d->page_bounds.y0;
    out->x1 = d->page_bounds.x1;
    out->y1 = d->page_bounds.y1;
    return 0;
}

int doc_render(doc *d, int page_no, const doc_view *view, uint8_t *dst, int w, int h, int stride,
               doc_cookie *cookie, doc_timing *t)
{
    if (load_page(d, page_no, t) < 0)
        return -1;

    double t0 = clock_ms();
    fz_context *ctx = d->ctx;
    fz_pixmap *pix = NULL;
    fz_device *dev = NULL;
    int rc = 0;

    fz_var(pix);
    fz_var(dev);
    fz_var(rc);
    fz_washi_abort = cookie && !d->opt.slow_abort ? &cookie->fz.abort : NULL;
    fz_try(ctx)
    {
        pix = fz_new_pixmap_with_data(ctx, fz_device_gray(ctx), w, h, NULL, 0, stride, dst);
        fz_clear_pixmap_with_value(ctx, pix, 255);
        fz_matrix ctm = fz_pre_translate(fz_scale(view->scale, view->scale), -view->x0, -view->y0);
        dev = fz_new_draw_device(ctx, ctm, pix);
        fz_run_page(ctx, d->page, dev, fz_identity, cookie ? &cookie->fz : NULL);
        fz_close_device(ctx, dev);
        if (cookie && __atomic_load_n(&cookie->fz.abort, __ATOMIC_RELAXED))
            rc = 1;
    }
    fz_always(ctx)
    {
        fz_washi_abort = NULL;
        fz_drop_device(ctx, dev);
        fz_drop_pixmap(ctx, pix);
    }
    fz_catch(ctx)
    {
        if (cookie && __atomic_load_n(&cookie->fz.abort, __ATOMIC_RELAXED)) {
            rc = 1; /* отмена посреди распаковки картинки */
        } else {
            set_error(d, "отрисовка", fz_caught_message(ctx));
            rc = -1;
        }
    }
    if (t)
        t->render_ms = clock_ms() - t0;
    return rc;
}

const char *doc_error(const doc *d)
{
    return d->err;
}

int doc_warning_count(const doc *d)
{
    return d->warnings;
}
