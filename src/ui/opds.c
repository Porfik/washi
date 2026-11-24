/* Каталоги: каталоги OPDS — листать, искать, скачивать книги.
 *
 * Экраны: список каталогов → лента (разделы и книги, листание, поиск) → карточка книги
 * (обложка, аннотация, форматы, «Скачать» → ход → «Читать»). Назад — стеком по каталогу.
 *
 * Сеть — в потоках: один поток на запрос (лента, поиск, скачивание) и один — на обложки
 * (список и карточка). Обмен — через мьютекс, забирается в tick. Разбор каталога —
 * src/net/opds_feed.c. Свои каталоги лежат в <папка данных>/opds.txt (название, адрес, логин
 * и пароль через табуляцию; логин и пароль — зашифрованные, src/base/cred.h).
 */
#define _DEFAULT_SOURCE
#include <mupdf/fitz.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../base/clock.h"
#include "../base/cred.h"
#include "../base/log.h"
#include "../net/net.h"
#include "../net/opds_feed.h"
#include "folderpick.h"
#include "page.h"
#include "widgets.h"

#define MAX_CATS 24
#define MAX_FRAMES 40
#define FEED_ROW_H 150
#define PER_PAGE 8       /* записей ленты на экране */
#define LIST_ROW_H UI_ROW_H
#define LIST_PER_PAGE 8  /* строк списка каталогов на экране */
#define BTN_H 100        /* кнопки и пилюли — не ниже */
#define PAGER_H 100
#define MAX_FMT 6
#define COVERS 48        /* обложек в памяти */
#define CV_MAX_W 300      /* обложки хранятся не больше этого */
#define CV_MAX_H 400
#define FEED_MAX (4 << 20)
#define FRAMES_BYTES (24 << 20) /* лент в памяти не больше */
#define ACTION_H 300     /* зона кнопки внизу карточки книги */
#define PROGRESS_MS 2000 /* ход загрузки рисуется не чаще */
#define PROGRESS_STEP 0.05f /* или раньше, когда полоса сдвинулась на столько */
#define COVER_MS 1000    /* обложки рисуются не чаще */

typedef cred_cat catalog;

/* Готовые каталоги: свободные библиотеки, отвечавшие при проверке. Любой другой человек
 * добавляет сам по адресу. */
static const catalog presets[] = {
    { "Ebooksgratuits — французская классика", "https://www.ebooksgratuits.com/opds/", "", "", 0 },
    { "Wolne Lektury — польская классика", "https://wolnelektury.pl/opds/", "", "", 0 },
};
#define NPRESETS ((int)(sizeof presets / sizeof *presets))

/* ---- Запросы ---- */

enum { J_FEED, J_SEARCH, J_DOWNLOAD };
/* Зачем запрос: что сделать с лентой, когда она пришла. */
enum { P_ROOT, P_ADD, P_ENTER, P_NEXT, P_PREV, P_REFRESH, P_SEARCH, P_DOWNLOAD };

typedef struct {
    int kind, purpose;
    char url[2048];  /* лента, шаблон поиска или файл */
    char osd[2048];  /* поиск: описание OpenSearch, если шаблона нет */
    char term[256];  /* поиск: что искали */
    char login[96], pass[96];
    char dir[1024], name[256], ext[16]; /* скачивание: папка, имя без расширения, расширение по ленте */
} job_spec;

typedef struct opds_page opds_page;

typedef struct job {
    job_spec sp;
    opds_page *r;
    volatile int cancel;
    int abandoned, done;
    int ok;
    long status;
    char error[256];
    opds_feed *feed;
    char final_url[2048];
    char path[1024]; /* скачанная книга */
} job;

/* ---- Обложки ---- */

enum { CV_FREE, CV_PENDING, CV_LOADING, CV_READY, CV_FAILED };

typedef struct {
    char *url;
    uint8_t *px; /* серый, w×h */
    int w, h;
    int state;
    unsigned gen, used;
} cover;

/* ---- Ленты ---- */

typedef struct {
    char url[2048];
    opds_feed *feed; /* NULL — выгружена из памяти, при возврате загрузится снова */
    int page;        /* экран ленты, с 0 */
    int dir;         /* 0 — раздел, +1 — пришли кнопкой «дальше», −1 — «назад» */
    int nent;        /* записей в ленте (когда сама выгружена) */
    char title[160];
    char term[256];  /* поиск: что искали (пусто — не результаты поиска) */
} frame;

enum { V_LIST, V_FEED, V_BOOK };
enum { OV_NONE, OV_LOADING, OV_ERROR, OV_CONFIRM, OV_STOP };
enum { DL_IDLE, DL_RUN, DL_DONE, DL_FAIL };
enum { KB_NONE, KB_ADDR, KB_SEARCH, KB_LOGIN, KB_PASS, KB_ELOGIN, KB_EPASS }; /* E — «Изменить вход» */

struct opds_page {
    page base;
    page_env env;
    char file[1100];
    pthread_mutex_t mu;
    pthread_cond_t cv, cov_cv;
    int running; /* потоков (запрос и обложки), ещё не закончивших */

    catalog mine[MAX_CATS];
    int nmine;
    catalog cat; /* открытый */
    int cat_idx; /* в mine, −1 — готовый или ещё не добавленный */
    int view, ov;
    int lpage;
    char msg[256];
    int can_retry;
    int confirm_idx;
    int stop_leave; /* «Остановить?» спросили по просьбе хаба (уход со страницы), а не по «‹» */
    int nohttp;
    char nohttp_why[160];
    int net_ok;
    int net_wait;   /* идёт подключение к Wi‑Fi (pf_net_poll в tick), потом — r->last */
    int net_choose; /* известной сети рядом нет: «Выбрать сеть» — окно прошивки */

    frame frames[MAX_FRAMES];
    int nframes;
    job *job;
    job_spec last; /* для «Повторить» и входа с паролем */
    int auth_tries;

    /* ввод */
    int kb_want, kb_got, kb_next, in_kb;
    char kb_text[512];
    char tmp_login[96];
    int pend; /* что вернуть из tick */

    /* книга */
    const opds_entry *book;
    opds_format fm[MAX_FMT];
    int nfm, fmt;
    struct {
        int state;
        char err[256];
        char path[1024];
        char name[256];
        long long done, total;
        long long shown_done, shown_total;
        double last_draw;
    } dl;

    /* обложки */
    cover covs[COVERS];
    unsigned cov_gen, cov_tick;
    pthread_t cov_th;
    int cov_started, cov_quit, cov_new;
    volatile int cov_abort;
    char cov_url[512], cov_login[96], cov_pass[96]; /* адрес и вход открытого каталога — для потока обложек */

    /* экран: области касаний */
    ui_header_hits hits;
    ui_rect rows[LIST_PER_PAGE];
    int row_item[LIST_PER_PAGE];
    int nrows;
    ui_rect search, pg_prev, pg_next, rows_rect, cover_rect;
    ui_rect fmt_hit[MAX_FMT];
    ui_rect btn1, btn2, ov1, ov2, remove_btn, login_btn, logout_btn;
    ui_rect dir_btn; /* «Сохранится в: …» — выбор папки */
    dirsel *ds;      /* общая папка загрузок или своя для каталогов (opds_dir.txt) */
    int has_search;
    double cover_draw;
};

/* Копия строки с обрезкой по размеру приёмника. */
static void scpy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n)
        l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

/* ---- Адреса ---- */

/* «host:port» из адреса, в нижнем регистре. */
static void url_host(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = memchr(p, '@', strcspn(p, "/"));
    if (at)
        p = at + 1;
    size_t n = strcspn(p, "/?#");
    if (n >= len)
        n = len - 1;
    for (size_t i = 0; i < n; i++)
        out[i] = (char)((p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i]);
    out[n] = 0;
}

static int same_host(const char *a, const char *b)
{
    char x[256], y[256];
    url_host(a, x, sizeof x);
    url_host(b, y, sizeof y);
    return !strcmp(x, y);
}

/* Адрес без «http://» и конца «/» — для подписей. */
static void short_url(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    snprintf(out, len, "%s", p ? p + 3 : url);
    size_t n = strlen(out);
    while (n && out[n - 1] == '/')
        out[--n] = 0;
}

/* Что ввёл человек → адрес: без схемы — http для адресов в своей сети (IP, localhost, порт, имя
 * без точки), иначе https. */
static void norm_url(const char *in, char *out, size_t len)
{
    while (*in == ' ' || *in == '\t')
        in++;
    size_t n = strlen(in);
    while (n && (in[n - 1] == ' ' || in[n - 1] == '\t' || in[n - 1] == '\n'))
        n--;
    char tmp[480];
    snprintf(tmp, sizeof tmp, "%.*s", (int)n, in);
    if (strstr(tmp, "://")) {
        snprintf(out, len, "%s", tmp);
        return;
    }
    size_t hl = strcspn(tmp, "/?#");
    int port = 0, digits_only = 1, dots = 0;
    for (size_t i = 0; i < hl; i++) {
        if (tmp[i] == ':')
            port = 1;
        if (tmp[i] == '.')
            dots++;
        if (!(tmp[i] >= '0' && tmp[i] <= '9') && tmp[i] != '.' && tmp[i] != ':')
            digits_only = 0;
    }
    int local = port || digits_only || !dots || !strncasecmp(tmp, "localhost", 9);
    snprintf(out, len, "%s://%s", local ? "http" : "https", tmp);
}

/* ---- Свои каталоги ---- */

static void cats_load(opds_page *r)
{
    int rewrite;
    r->nmine = cred_cats_load(r->file, r->mine, MAX_CATS, &rewrite);
    if (rewrite) /* старое открытое — сразу зашифрованным */
        cred_cats_save(r->file, r->mine, r->nmine);
}

static int cats_save(opds_page *r)
{
    return cred_cats_save(r->file, r->mine, r->nmine);
}

/* ---- Потоки: общее ---- */

static void job_free(job *j)
{
    opds_feed_free(j->feed);
    free(j);
}

/* Забыть текущий запрос: он прервётся и сам освободит память, когда закончит. */
static void job_abandon(opds_page *r)
{
    pthread_mutex_lock(&r->mu);
    job *j = r->job;
    r->job = NULL;
    if (j) {
        j->cancel = 1;
        if (j->done)
            job_free(j);
        else
            j->abandoned = 1;
    }
    pthread_mutex_unlock(&r->mu);
}

static void dl_progress(void *user, long long done, long long total)
{
    job *j = user;
    opds_page *r = j->r;
    pthread_mutex_lock(&r->mu);
    if (r->job == j) { /* забытый запрос ход не показывает */
        r->dl.done = done;
        r->dl.total = total;
    }
    pthread_mutex_unlock(&r->mu);
}

static void set_http_error(job *j, const net_response *rs)
{
    j->status = rs->status;
    switch (rs->status) {
    case 401: snprintf(j->error, sizeof j->error, "Нужен логин и пароль"); break;
    case 403: snprintf(j->error, sizeof j->error, "Доступ закрыт"); break;
    case 404: snprintf(j->error, sizeof j->error, "Такой страницы на сервере нет"); break;
    default: scpy(j->error, sizeof j->error, *rs->error ? rs->error : "Не получилось"); break;
    }
}

/* GET; ответ в памяти. 0 — получен. */
static int fetch(job *j, const char *url, size_t max, net_response *rs)
{
    static const char *const hdr[] = { "Accept: application/atom+xml, application/xml;q=0.9, text/xml;q=0.8, */*;q=0.5", NULL };
    net_request rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.headers = hdr;
    rq.max_body = max;
    rq.timeout_s = 40;
    rq.cancel = &j->cancel;
    if (*j->sp.login) {
        rq.login = j->sp.login;
        rq.password = j->sp.pass;
    }
    int rc = net_perform(&rq, rs);
    if (rc != 0)
        set_http_error(j, rs);
    return rc;
}

static void run_feed(job *j, const char *url)
{
    net_response rs;
    if (fetch(j, url, FEED_MAX, &rs) != 0) {
        net_response_free(&rs);
        return;
    }
    int js = 0, html = 0;
    int xml = opds_looks_like_xml(rs.body, rs.len, &js, &html);
    if (strstr(rs.content_type, "json"))
        js = 1;
    if (!xml) {
        if (js)
            snprintf(j->error, sizeof j->error, "Каталог OPDS 2 (JSON) пока не поддерживается");
        else if (html)
            snprintf(j->error, sizeof j->error, "По этому адресу страница сайта, а не каталог. Проверьте адрес");
        else
            snprintf(j->error, sizeof j->error, "Это не каталог OPDS");
        net_response_free(&rs);
        return;
    }
    scpy(j->final_url, sizeof j->final_url, *rs.final_url ? rs.final_url : url);
    j->feed = opds_feed_parse(rs.body, rs.len, j->final_url);
    if (!j->feed)
        snprintf(j->error, sizeof j->error, "Не удалось прочитать каталог: это не OPDS");
    else
        j->ok = 1;
    net_response_free(&rs);
}

static void run_search(job *j)
{
    char tmpl[2048];
    if (*j->sp.url) {
        scpy(tmpl, sizeof tmpl, j->sp.url);
    } else {
        net_response rs;
        if (fetch(j, j->sp.osd, 256 << 10, &rs) != 0) {
            net_response_free(&rs);
            return;
        }
        int rc = opds_osd_template(rs.body, rs.len, *rs.final_url ? rs.final_url : j->sp.osd, tmpl, sizeof tmpl);
        net_response_free(&rs);
        if (rc != 0) {
            snprintf(j->error, sizeof j->error, "В этом каталоге нет поиска");
            return;
        }
    }
    char url[2048];
    opds_search_url(tmpl, j->sp.term, url, sizeof url);
    run_feed(j, url);
}

/* Расширение после скачивания: по содержимому, потом по имени от сервера, по ленте, по типу. */
static const char *final_ext(const char *tmp, const net_response *rs, const char *expect)
{
    unsigned char head[96];
    size_t n = 0;
    FILE *f = fopen(tmp, "rb");
    if (f) {
        n = fread(head, 1, sizeof head, f);
        fclose(f);
    }
    const char *sniff = opds_ext_sniff(head, n);
    if (*sniff && strcmp(sniff, ".zip"))
        return sniff;
    const char *e = opds_ext_for_path(rs->filename);
    if (*e)
        return e;
    if (strcmp(expect, ".bin"))
        return expect;
    e = opds_ext_for_type(rs->content_type);
    return *e ? e : (*sniff ? sniff : ".bin");
}

static void run_download(job *j)
{
    job_spec *sp = &j->sp;
    if (net_mkdirs(sp->dir) != 0) {
        snprintf(j->error, sizeof j->error, "Не удалось создать папку для книг");
        return;
    }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s/.opds-dl.tmp", sp->dir);
    net_request rq;
    memset(&rq, 0, sizeof rq);
    rq.url = sp->url;
    rq.out_path = tmp;
    rq.cancel = &j->cancel;
    rq.progress = dl_progress;
    rq.user = j;
    if (*sp->login) {
        rq.login = sp->login;
        rq.password = sp->pass;
    }
    net_response rs;
    if (net_perform(&rq, &rs) != 0) {
        set_http_error(j, &rs);
        net_response_free(&rs);
        return;
    }
    if (!strncasecmp(rs.content_type, "text/html", 9)) {
        snprintf(j->error, sizeof j->error, "Сервер прислал страницу вместо книги");
        remove(tmp);
        net_response_free(&rs);
        return;
    }
    const char *ext = final_ext(tmp, &rs, sp->ext);
    /* имя: от сервера (если у него есть своё), иначе «Автор — Название» из ленты */
    char base[256];
    scpy(base, sizeof base, sp->name);
    if (*rs.filename && *opds_ext_for_path(rs.filename)) {
        scpy(base, sizeof base, rs.filename);
        size_t bl = strlen(base), el = strlen(opds_ext_for_path(rs.filename));
        if (strcmp(opds_ext_for_path(rs.filename), ".djvu") && bl > el)
            base[bl - el] = 0;
        else if (bl > 5 && !strcasecmp(base + bl - 5, ".djvu"))
            base[bl - 5] = 0;
        else if (bl > 4 && !strcasecmp(base + bl - 4, ".djv"))
            base[bl - 4] = 0;
    }
    char name[300];
    snprintf(name, sizeof name, "%s%s", base, ext);
    char dest[1100];
    if (net_unique_path(sp->dir, name, dest, sizeof dest) != 0 || rename(tmp, dest) != 0) {
        snprintf(j->error, sizeof j->error, "Не удалось сохранить книгу в %.150s", sp->dir);
        remove(tmp);
        net_response_free(&rs);
        return;
    }
    scpy(j->path, sizeof j->path, dest);
    j->ok = 1;
    net_response_free(&rs);
}

static void *job_main(void *arg)
{
    job *j = arg;
    opds_page *r = j->r;
    switch (j->sp.kind) {
    case J_FEED: run_feed(j, j->sp.url); break;
    case J_SEARCH: run_search(j); break;
    default: run_download(j); break;
    }
    if (j->cancel && !j->ok)
        snprintf(j->error, sizeof j->error, "Отменено");
    pthread_mutex_lock(&r->mu);
    if (j->abandoned)
        job_free(j);
    else
        j->done = 1;
    r->running--;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

static int job_start(opds_page *r, const job_spec *sp)
{
    job_abandon(r);
    job *j = calloc(1, sizeof *j);
    if (!j)
        return -1;
    j->sp = *sp;
    j->r = r;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_mutex_lock(&r->mu);
    r->running++;
    r->job = j;
    pthread_mutex_unlock(&r->mu);
    pthread_t th;
    int rc = pthread_create(&th, &at, job_main, j);
    pthread_attr_destroy(&at);
    if (rc != 0) {
        pthread_mutex_lock(&r->mu);
        r->running--;
        r->job = NULL;
        pthread_mutex_unlock(&r->mu);
        free(j);
        return -1;
    }
    return 0;
}

/* ---- Обложки ---- */

/* Серая картинка не больше CV_MAX_W×CV_MAX_H. 0 — получилось. */
static int decode_cover(const unsigned char *data, size_t n, uint8_t **out, int *ow, int *oh)
{
    fz_context *ctx = fz_new_context(NULL, NULL, 16 << 20);
    if (!ctx)
        return -1;
    fz_buffer *buf = NULL;
    fz_image *img = NULL;
    fz_pixmap *pm = NULL, *gray = NULL;
    int rc = -1;
    fz_var(buf);
    fz_var(img);
    fz_var(pm);
    fz_var(gray);
    fz_var(rc);
    fz_try(ctx)
    {
        buf = fz_new_buffer_from_copied_data(ctx, data, n);
        img = fz_new_image_from_buffer(ctx, buf);
        if ((long long)img->w * img->h > 30000000LL)
            fz_throw(ctx, FZ_ERROR_GENERIC, "слишком большая обложка");
        pm = fz_get_unscaled_pixmap_from_image(ctx, img);
        gray = fz_convert_pixmap(ctx, pm, fz_device_gray(ctx), NULL, NULL, fz_default_color_params, 0);
        int w = gray->w, h = gray->h, f = 1;
        while (w / f > CV_MAX_W || h / f > CV_MAX_H)
            f++;
        int dw = w / f, dh = h / f;
        if (dw < 1 || dh < 1)
            fz_throw(ctx, FZ_ERROR_GENERIC, "пустая обложка");
        uint8_t *px = malloc((size_t)dw * (size_t)dh);
        if (!px)
            fz_throw(ctx, FZ_ERROR_GENERIC, "нет памяти");
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                unsigned sum = 0;
                for (int yy = 0; yy < f; yy++) {
                    const unsigned char *row = gray->samples + (size_t)(y * f + yy) * (size_t)gray->stride + (size_t)(x * f) * (size_t)gray->n;
                    for (int xx = 0; xx < f; xx++)
                        sum += row[xx * gray->n];
                }
                px[(size_t)y * (size_t)dw + (size_t)x] = (uint8_t)(sum / (unsigned)(f * f));
            }
        *out = px;
        *ow = dw;
        *oh = dh;
        rc = 0;
    }
    fz_always(ctx)
    {
        fz_drop_pixmap(ctx, gray);
        fz_drop_pixmap(ctx, pm);
        fz_drop_image(ctx, img);
        fz_drop_buffer(ctx, buf);
    }
    fz_catch(ctx)
    {
        rc = -1;
    }
    fz_drop_context(ctx);
    return rc;
}

static void *cover_main(void *arg)
{
    opds_page *r = arg;
    pthread_mutex_lock(&r->mu);
    for (;;) {
        if (r->cov_quit)
            break;
        cover *c = NULL;
        for (int i = 0; i < COVERS; i++)
            if (r->covs[i].state == CV_PENDING && r->covs[i].gen == r->cov_gen && (!c || r->covs[i].used < c->used))
                c = &r->covs[i];
        if (!c) {
            pthread_cond_wait(&r->cov_cv, &r->mu);
            continue;
        }
        c->state = CV_LOADING;
        char url[2048], login[96] = "", pass[96] = "";
        scpy(url, sizeof url, c->url);
        if (*r->cov_login && same_host(r->cov_url, url)) {
            scpy(login, sizeof login, r->cov_login);
            scpy(pass, sizeof pass, r->cov_pass);
        }
        pthread_mutex_unlock(&r->mu);

        uint8_t *px = NULL;
        int w = 0, h = 0, ok = 0;
        net_request rq;
        memset(&rq, 0, sizeof rq);
        rq.url = url;
        rq.max_body = 3 << 20;
        rq.timeout_s = 25;
        rq.cancel = &r->cov_abort;
        if (*login) {
            rq.login = login;
            rq.password = pass;
        }
        net_response rs;
        if (net_perform(&rq, &rs) == 0 && rs.body)
            ok = decode_cover((const unsigned char *)rs.body, rs.len, &px, &w, &h) == 0;
        net_response_free(&rs);

        pthread_mutex_lock(&r->mu);
        c->px = px;
        c->w = w;
        c->h = h;
        c->state = ok ? CV_READY : CV_FAILED;
        if (!ok && r->cov_abort) { /* оборвали при уходе со страницы — не ошибка обложки */
            free(c->url);
            c->url = NULL;
            c->state = CV_FREE;
        }
        r->cov_new = 1;
    }
    r->running--;
    pthread_cond_broadcast(&r->cv);
    pthread_mutex_unlock(&r->mu);
    return NULL;
}

/* Обложка по адресу: готовая или NULL (тогда заказана, придёт позже). */
static const cover *cover_get(opds_page *r, const char *url)
{
    if (!url || !*url)
        return NULL;
    pthread_mutex_lock(&r->mu);
    cover *hit = NULL, *free_slot = NULL, *old = NULL;
    for (int i = 0; i < COVERS; i++) {
        cover *c = &r->covs[i];
        if (c->state == CV_FREE) {
            free_slot = free_slot ? free_slot : c;
            continue;
        }
        if (!strcmp(c->url, url))
            hit = c;
        else if ((c->state == CV_READY || c->state == CV_FAILED) && (!old || c->used < old->used))
            old = c;
    }
    cover *res = NULL;
    if (!hit) {
        hit = free_slot ? free_slot : old;
        if (hit) {
            free(hit->url);
            free(hit->px);
            memset(hit, 0, sizeof *hit);
            hit->url = strdup(url);
            hit->state = hit->url ? CV_PENDING : CV_FREE;
        }
    }
    if (hit && hit->state != CV_FREE) {
        hit->used = ++r->cov_tick;
        if (hit->state == CV_PENDING) {
            hit->gen = r->cov_gen;
            if (!r->cov_started && !r->cov_quit) {
                pthread_attr_t at;
                pthread_attr_init(&at);
                pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
                r->running++;
                if (pthread_create(&r->cov_th, &at, cover_main, r) == 0)
                    r->cov_started = 1;
                else
                    r->running--;
                pthread_attr_destroy(&at);
            }
            pthread_cond_signal(&r->cov_cv);
        } else if (hit->state == CV_READY) {
            res = hit;
        }
    }
    pthread_mutex_unlock(&r->mu);
    return res;
}

/* Новый «выпуск» заказов: заказанное раньше и не начатое больше не нужно. */
static void cover_next_gen(opds_page *r)
{
    pthread_mutex_lock(&r->mu);
    r->cov_gen++;
    scpy(r->cov_url, sizeof r->cov_url, r->cat.url);
    scpy(r->cov_login, sizeof r->cov_login, r->cat.login);
    scpy(r->cov_pass, sizeof r->cov_pass, r->cat.pass);
    pthread_mutex_unlock(&r->mu);
}

/* ---- Ленты в памяти ---- */

static frame *top(opds_page *r)
{
    return r->nframes ? &r->frames[r->nframes - 1] : NULL;
}

static void frames_clear(opds_page *r)
{
    for (int i = 0; i < r->nframes; i++)
        opds_feed_free(r->frames[i].feed);
    r->nframes = 0;
    r->book = NULL;
}

static void frames_trim(opds_page *r)
{
    long long total = 0;
    for (int i = 0; i < r->nframes; i++)
        if (r->frames[i].feed)
            total += r->frames[i].feed->bytes;
    for (int i = 0; i + 1 < r->nframes && total > FRAMES_BYTES; i++)
        if (r->frames[i].feed) {
            total -= r->frames[i].feed->bytes;
            opds_feed_free(r->frames[i].feed);
            r->frames[i].feed = NULL;
        }
}

static int frame_push(opds_page *r, const char *url, opds_feed *feed, int dir, const char *title, const char *term)
{
    if (r->nframes >= MAX_FRAMES)
        return -1;
    frame *f = &r->frames[r->nframes++];
    memset(f, 0, sizeof *f);
    scpy(f->url, sizeof f->url, url);
    f->feed = feed;
    f->nent = feed ? feed->nentries : 0;
    f->dir = dir;
    scpy(f->term, sizeof f->term, term ? term : "");
    scpy(f->title, sizeof f->title, title && *title ? title : "Каталог");
    frames_trim(r);
    return 0;
}

static void frame_pop(opds_page *r)
{
    if (!r->nframes)
        return;
    opds_feed_free(r->frames[--r->nframes].feed);
}

/* Лента, чей поиск доступен на этом экране: своя, а нет — ближайшая выше по стеку (в результатах
 * поиска и в разделах поле поиска остаётся). */
static const opds_feed *search_source(const opds_page *r)
{
    for (int i = r->nframes - 1; i >= 0; i--) {
        const opds_feed *f = r->frames[i].feed;
        if (f && (*f->search || *f->search_desc))
            return f;
    }
    return NULL;
}

/* Корень своего каталога: внизу него кнопки «Изменить вход», «Выйти» и «Убрать из моих каталогов», им нужно место. */
static int is_own_root(const opds_page *r, int idx)
{
    return idx == 0 && r->cat_idx >= 0 && r->nframes && r->frames[0].dir == 0;
}

static int frame_per(const opds_page *r, int idx)
{
    return is_own_root(r, idx) ? PER_PAGE - 2 : PER_PAGE; /* внизу два ряда кнопок */
}

/* Экранов в ленте кадра idx. */
static int frame_npages(const opds_page *r, int idx)
{
    const frame *f = &r->frames[idx];
    int ne = f->feed ? f->feed->nentries : f->nent, per = frame_per(r, idx);
    int n = (ne + per - 1) / per;
    return n < 1 ? 1 : n;
}

/* Сквозная нумерация экранов: «страницы каталога на сервере» листаются как одна лента.
 * cur — номер текущего экрана, total — сколько экранов известно; *more — на сервере есть ещё. */
static void frame_numbers(const opds_page *r, int *cur, int *total, int *more)
{
    int top_i = r->nframes - 1;
    const frame *f = &r->frames[top_i];
    int before = 0, all = frame_npages(r, top_i);
    for (int j = top_i; j > 0 && r->frames[j].dir == 1; j--) {
        before += frame_npages(r, j - 1);
        all += frame_npages(r, j - 1);
    }
    for (int j = top_i; j > 0 && r->frames[j].dir == -1; j--)
        all += frame_npages(r, j - 1);
    *cur = before + f->page + 1;
    *total = all;
    *more = f->feed && *f->feed->next && f->dir != -1;
}

/* ---- Запуск запросов ---- */

static void set_error(opds_page *r, const char *msg, int retry)
{
    scpy(r->msg, sizeof r->msg, msg);
    r->ov = OV_ERROR;
    r->can_retry = retry;
}

/* Логин и пароль каталога — только на его собственный адрес. */
static void spec_auth(opds_page *r, job_spec *sp, const char *url)
{
    sp->login[0] = sp->pass[0] = 0;
    if (*r->cat.login && same_host(r->cat.url, url)) {
        scpy(sp->login, sizeof sp->login, r->cat.login);
        scpy(sp->pass, sizeof sp->pass, r->cat.pass);
    }
}

/* Запустить запрос: Wi‑Fi (если нужно), поток, окно «Загрузка». Возвращает флаги PAGE_*. */
static int start_job(opds_page *r, const job_spec *sp)
{
    if (r->nohttp) {
        set_error(r, r->nohttp_why, 0);
        return PAGE_REDRAW;
    }
    if (!r->net_ok) {
        r->last = *sp;
        if (r->net_choose) {
            /* «Выбрать сеть»: окно прошивки (блокирует, пока человек выбирает), потом — с миганием. */
            r->net_choose = 0;
            int rc = pf_net_connect();
            r->env.redraw(r->env.user, 1);
            if (rc != 0) {
                set_error(r, "Нет подключения к Wi‑Fi. Выберите сеть и повторите", 1);
                r->net_choose = 1;
                return PAGE_REDRAW;
            }
        } else {
            int rc = pf_net_start();
            if (rc == 0) { /* подключаемся в фоне: окно «Подключаюсь…», ждём в tick */
                r->net_wait = 1;
                r->ov = OV_LOADING;
                return PAGE_REDRAW;
            }
            if (rc < 0) {
                set_error(r, "Известной сети Wi‑Fi рядом нет. Выберите сеть", 1);
                r->net_choose = 1;
                return PAGE_REDRAW;
            }
        }
        r->net_ok = 1;
    }
    r->last = *sp;
    if (job_start(r, sp) != 0) {
        set_error(r, "Не хватило памяти", 0);
        return PAGE_REDRAW;
    }
    log_msg("каталоги: запрос %s", sp->kind == J_SEARCH ? sp->term : sp->url);
    pf_keep_awake(60);
    if (sp->kind == J_DOWNLOAD) {
        r->dl.state = DL_RUN;
        r->dl.done = 0;
        r->dl.total = -1;
        r->dl.shown_done = -1;
        r->dl.shown_total = 0;
        r->dl.last_draw = clock_ms();
        r->dl.err[0] = 0;
    } else {
        r->ov = OV_LOADING;
    }
    return PAGE_REDRAW;
}

static int fetch_feed(opds_page *r, int purpose, const char *url)
{
    job_spec sp;
    memset(&sp, 0, sizeof sp);
    sp.kind = J_FEED;
    sp.purpose = purpose;
    scpy(sp.url, sizeof sp.url, url);
    spec_auth(r, &sp, url);
    r->auth_tries = 0;
    return start_job(r, &sp);
}

static int open_catalog(opds_page *r, const catalog *c, int idx)
{
    frames_clear(r);
    r->cat = *c;
    r->cat_idx = idx;
    r->book = NULL;
    return fetch_feed(r, P_ROOT, c->url);
}

static int ask(opds_page *r, int what, const char *title, const char *initial, int maxlen);

/* Результат запроса: забирается из tick. Возвращает флаги PAGE_*. */
static int job_finished_inner(opds_page *r, job *j)
{
    const job_spec sp = j->sp;
    int flags = PAGE_REDRAW;
    r->ov = OV_NONE;
    if (!j->ok) {
        int cancelled = !strcmp(j->error, "Отменено");
        if (!j->status && !cancelled)
            r->net_ok = 0; /* ответа не было — при повторе подключиться к сети заново */
        log_msg("каталоги: не вышло (%ld): %s", j->status, j->error);
        if (sp.kind == J_DOWNLOAD) {
            r->dl.state = cancelled ? DL_IDLE : DL_FAIL;
            scpy(r->dl.err, sizeof r->dl.err, j->error);
            if (j->status == 401 && !r->auth_tries) {
                r->dl.state = DL_IDLE;
                r->kb_next = KB_LOGIN;
                r->auth_tries = 1;
            }
            return flags;
        }
        if (cancelled)
            return flags;
        if (j->status == 401 && r->auth_tries < 1) {
            r->auth_tries++;
            r->kb_next = KB_LOGIN;
            r->ov = OV_LOADING; /* пока спрашиваем вход */
            return flags;
        }
        if (j->status == 401)
            snprintf(j->error, sizeof j->error, "Неверный логин или пароль");
        set_error(r, j->error, 1);
        return flags;
    }
    r->auth_tries = 0;
    if (sp.kind == J_DOWNLOAD) {
        r->dl.state = DL_DONE;
        scpy(r->dl.path, sizeof r->dl.path, j->path);
        const char *slash = strrchr(j->path, '/');
        scpy(r->dl.name, sizeof r->dl.name, slash ? slash + 1 : j->path);
        log_msg("каталоги: скачано %s", j->path);
        return flags | PAGE_BOOKS;
    }
    opds_feed *feed = j->feed;
    j->feed = NULL;
    const char *url = *j->final_url ? j->final_url : sp.url;
    int rc = 0;
    switch (sp.purpose) {
    case P_ROOT:
    case P_ADD:
        frames_clear(r);
        if (sp.purpose == P_ADD) {
            /* новый каталог: название — из ленты, а нет его — адрес */
            catalog c = r->cat;
            if (*feed->title)
                scpy(c.name, sizeof c.name, feed->title);
            else
                short_url(c.url, c.name, sizeof c.name);
            int dup = -1;
            for (int i = 0; i < r->nmine; i++)
                if (!strcmp(r->mine[i].url, c.url))
                    dup = i;
            if (dup >= 0) {
                r->cat_idx = dup;
                r->cat = r->mine[dup];
            } else if (r->nmine < MAX_CATS) {
                r->mine[r->nmine] = c;
                r->cat_idx = r->nmine++;
                r->cat = c;
                if (cats_save(r) != 0)
                    log_msg("каталоги: не удалось записать %s", r->file);
            }
        }
        rc = frame_push(r, url, feed, 0, r->cat.name, "");
        break;
    case P_REFRESH:
        if (top(r)) {
            opds_feed_free(top(r)->feed);
            top(r)->feed = feed;
            top(r)->nent = feed->nentries;
        } else {
            rc = -1;
        }
        break;
    case P_PREV:
    case P_NEXT: { /* то же место каталога: заголовок прежний, без «(страница 2)» */
        char title[160];
        scpy(title, sizeof title, top(r) ? top(r)->title : feed->title);
        rc = frame_push(r, url, feed, sp.purpose == P_NEXT ? 1 : -1, title, top(r) ? top(r)->term : "");
        if (!rc && sp.purpose == P_PREV)
            top(r)->page = frame_npages(r, r->nframes - 1) - 1;
        break;
    }
    case P_SEARCH:
        /* новый запрос заменяет прежние результаты, а не ложится поверх них */
        if (top(r) && *top(r)->term) {
            while (r->nframes > 1 && top(r)->dir != 0)
                frame_pop(r);
            frame_pop(r);
        }
        rc = frame_push(r, url, feed, 0, *feed->title ? feed->title : sp.term, sp.term);
        break;
    default: /* P_ENTER */
        rc = frame_push(r, url, feed, 0, *feed->title ? feed->title : sp.term, "");
        break;
    }
    if (rc != 0) {
        opds_feed_free(feed);
        set_error(r, "Слишком глубоко: вернитесь назад", 0);
        return flags;
    }
    /* логин, который подошёл, запоминаем в каталоге */
    if (*sp.login && r->cat_idx >= 0 && (strcmp(r->mine[r->cat_idx].login, sp.login) || strcmp(r->mine[r->cat_idx].pass, sp.pass))) {
        scpy(r->mine[r->cat_idx].login, sizeof r->mine[r->cat_idx].login, sp.login);
        scpy(r->mine[r->cat_idx].pass, sizeof r->mine[r->cat_idx].pass, sp.pass);
        cats_save(r);
    }
    r->view = V_FEED;
    return flags;
}

static int job_finished(opds_page *r, job *j)
{
    /* уходили со страницы («Остановить?»), а передача закончилась сама — просто уходим */
    int leaving = r->ov == OV_STOP && r->stop_leave && j->sp.kind == J_DOWNLOAD;
    r->stop_leave = 0;
    int flags = job_finished_inner(r, j);
    return leaving ? flags | PAGE_CLOSE : flags;
}

/* ---- Ввод с клавиатуры ---- */

/* Новый вход открытого каталога (пустые — «Выйти»): в каталоге и в файле. Лента на экране остаётся. */
static void set_login(opds_page *r, const char *login, const char *pass)
{
    scpy(r->cat.login, sizeof r->cat.login, login);
    scpy(r->cat.pass, sizeof r->cat.pass, pass);
    r->cat.lost = 0;
    if (r->cat_idx >= 0 && r->cat_idx < r->nmine) {
        scpy(r->mine[r->cat_idx].login, sizeof r->mine[r->cat_idx].login, login);
        scpy(r->mine[r->cat_idx].pass, sizeof r->mine[r->cat_idx].pass, pass);
        r->mine[r->cat_idx].lost = 0;
        cats_save(r);
    }
    r->auth_tries = 0;
}

static int kb_poll(opds_page *r)
{
    if (!r->kb_got)
        return 0;
    int got = r->kb_got, what = r->kb_want;
    r->kb_got = 0;
    r->kb_want = KB_NONE;
    char text[512];
    scpy(text, sizeof text, r->kb_text);
    if (got != 1) {
        if (what == KB_ELOGIN || what == KB_EPASS) { /* «Изменить вход» отменили — всё как было */
            r->kb_next = 0;
            return PAGE_REDRAW;
        }
        if (what == KB_LOGIN || what == KB_PASS) {
            r->ov = OV_NONE;
            set_error(r, "Без логина и пароля каталог не откроется", 1);
        }
        return PAGE_REDRAW;
    }
    switch (what) {
    case KB_ADDR: {
        memset(&r->cat, 0, sizeof r->cat);
        norm_url(text, r->cat.url, sizeof r->cat.url);
        r->cat_idx = -1;
        frames_clear(r);
        job_spec sp;
        memset(&sp, 0, sizeof sp);
        sp.kind = J_FEED;
        sp.purpose = P_ADD;
        scpy(sp.url, sizeof sp.url, r->cat.url);
        r->auth_tries = 0;
        return start_job(r, &sp);
    }
    case KB_SEARCH: {
        const opds_feed *sf = search_source(r);
        if (!sf)
            return 0;
        job_spec sp;
        memset(&sp, 0, sizeof sp);
        sp.kind = J_SEARCH;
        sp.purpose = P_SEARCH;
        scpy(sp.term, sizeof sp.term, text);
        if (*sf->search)
            scpy(sp.url, sizeof sp.url, sf->search);
        else
            scpy(sp.osd, sizeof sp.osd, sf->search_desc);
        spec_auth(r, &sp, *sp.url ? sp.url : sp.osd);
        r->auth_tries = 0;
        return start_job(r, &sp);
    }
    case KB_LOGIN:
        scpy(r->tmp_login, sizeof r->tmp_login, text);
        r->kb_next = KB_PASS; /* следующее окно — из tick: не открывать окно из обработчика окна */
        return PAGE_REDRAW;
    case KB_ELOGIN:
        if (!*text) /* пустой ввод — не менять */
            return PAGE_REDRAW;
        scpy(r->tmp_login, sizeof r->tmp_login, text);
        r->kb_next = KB_EPASS;
        return PAGE_REDRAW;
    case KB_EPASS:
        if (!*text)
            return PAGE_REDRAW;
        set_login(r, r->tmp_login, text);
        return PAGE_REDRAW;
    case KB_PASS: {
        scpy(r->cat.login, sizeof r->cat.login, r->tmp_login);
        scpy(r->cat.pass, sizeof r->cat.pass, text);
        job_spec sp = r->last;
        spec_auth(r, &sp, sp.kind == J_SEARCH && !*sp.url ? sp.osd : sp.url);
        r->ov = OV_NONE;
        return start_job(r, &sp);
    }
    default: return 0;
    }
}

static void kb_done(const char *text, void *user)
{
    opds_page *r = user;
    if (text) {
        scpy(r->kb_text, sizeof r->kb_text, text);
        r->kb_got = 1;
    } else {
        r->kb_got = 2;
    }
    if (!r->in_kb) { /* окно прошивки закрылось не внутри нашего вызова */
        r->env.redraw(r->env.user, 1);
        r->pend |= kb_poll(r);
    }
}

static int ask(opds_page *r, int what, const char *title, const char *initial, int maxlen)
{
    r->kb_want = what;
    r->kb_got = 0;
    r->in_kb = 1;
    pf_keyboard(title, initial, maxlen, kb_done, r);
    r->in_kb = 0;
    int f = kb_poll(r);
    if (f & PAGE_REDRAW) { /* поверх был экран прошивки: рисуем заново, с миганием */
        r->env.redraw(r->env.user, 1);
        f &= ~PAGE_REDRAW;
    }
    return f;
}

/* ---- Рисование ---- */

static void fill_bg(opds_page *r, pf_screen *s)
{
    (void)r;
    gfx_fill(s, 0, UI_STATUS_H, s->width, ui_content_bottom(s) - UI_STATUS_H, UI_BG);
}

/* Формат с понятным именем (у остальных подписи «Файл» нет — не показываем). */
static int known_fmt(const opds_format *f)
{
    return strcmp(f->ext, ".bin") != 0;
}

/* Куда ляжет книга: путь относительно библиотеки, а не машинный. */
static void save_where(const opds_page *r, char *out, size_t len)
{
    if (dirsel_custom(r->ds)) {
        dirsel_label(r->ds, out, len);
        return;
    }
    const char *dir = r->env.download_dir ? r->env.download_dir : "", *root = r->env.library_root;
    size_t rl = root ? strlen(root) : 0;
    while (rl && root[rl - 1] == '/')
        rl--;
    if (rl && !strncmp(dir, root, rl) && dir[rl] == '/' && dir[rl + 1]) {
        snprintf(out, len, "%s", dir + rl + 1);
    } else {
        const char *slash = strrchr(dir, '/');
        snprintf(out, len, "%s", slash && slash[1] ? slash + 1 : dir);
    }
}

static const char *view_title(const opds_page *r)
{
    if (r->view == V_FEED && r->nframes)
        return r->frames[r->nframes - 1].title;
    if (r->view == V_BOOK)
        return "О книге";
    return "Каталоги";
}

static void draw_note(opds_page *r, pf_screen *s, int y, const char *text, int lines)
{
    gfx_style hs = { FONT_REGULAR, 34, 0x44 };
    gfx_text_wrap(r->env.g, s, UI_MARGIN + 8, y + 44, s->width - 2 * UI_MARGIN - 16, 46, lines, hs, text);
}

/* Строки списка каталогов: свои, «Добавить», готовые. */
static int list_count(const opds_page *r)
{
    return r->nmine + 1 + NPRESETS;
}

static void draw_list(opds_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int n = list_count(r), pages = (n + LIST_PER_PAGE - 1) / LIST_PER_PAGE;
    if (r->lpage >= pages)
        r->lpage = pages - 1;
    if (r->lpage < 0)
        r->lpage = 0;
    r->hits = ui_header(g, s, "Каталоги", 1, r->lpage, pages);
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, y = ui_content_top();
    int first = r->lpage * LIST_PER_PAGE, last = first + LIST_PER_PAGE < n ? first + LIST_PER_PAGE : n;
    r->nrows = 0;
    for (int i = first; i < last;) {
        int mine = i <= r->nmine; /* свои + «Добавить» */
        int j = i;
        while (j < last && (j <= r->nmine) == mine)
            j++;
        ui_caption(g, s, x + 8, y + 40, mine ? "Мои каталоги" : "Готовые каталоги");
        y += 58;
        ui_card(g, s, x, y, w, (j - i) * LIST_ROW_H);
        for (int k = i; k < j; k++, y += LIST_ROW_H) {
            int cy = y + LIST_ROW_H / 2, right = x + w - 40;
            char sub[600] = "";
            const char *title;
            if (k < r->nmine) {
                title = r->mine[k].name;
                short_url(r->mine[k].url, sub, sizeof sub);
            } else if (k == r->nmine) {
                title = "Добавить каталог";
                snprintf(sub, sizeof sub, "по адресу: Calibre, calibre-web, COPS и другие");
            } else {
                title = presets[k - r->nmine - 1].name;
                short_url(presets[k - r->nmine - 1].url, sub, sizeof sub);
            }
            if (k == r->nmine) {
                gfx_icon(g, s, ICON_PLUS, x + 40, cy - 24, 48, 0, UI_BLACK);
            } else {
                gfx_icon(g, s, ICON_NEXT, right - 40, cy - 20, 40, 0, UI_TEXT3);
                right -= 56;
            }
            int tx = k == r->nmine ? x + 112 : x + 40;
            gfx_style ts = { FONT_REGULAR, 40, UI_BLACK }, ss = { FONT_REGULAR, 32, UI_TEXT2 };
            gfx_text_fit(g, s, tx, cy - 8, right - tx, ts, title);
            gfx_text_fit_middle(g, s, tx, cy + 38, right - tx, ss, sub);
            if (k + 1 < j)
                gfx_fill(s, x + 40, y + LIST_ROW_H - 2, w - 40, 2, UI_FAINT);
            r->rows[r->nrows] = (ui_rect){ x, y, w, LIST_ROW_H };
            r->row_item[r->nrows++] = k;
        }
        y += 16;
        i = j;
    }
    if (r->lpage == pages - 1 && y + 240 < ui_content_bottom(s))
        draw_note(r, s, y,
                  "Свой каталог: в Calibre — «Сервер контента», адрес вида http://192.168.1.5:8080/opds; "
                  "calibre-web — /opds; COPS — /feed.php. Устройство и сервер должны быть в одной сети Wi‑Fi.",
                  5);
}

/* Обложка в рамке x,y,w,h: готовая (вписана) или пустое место со значком. */
static void draw_thumb(opds_page *r, pf_screen *s, int x, int y, int w, int h, const char *url, const char *url2)
{
    gfx *g = r->env.g;
    const cover *c = cover_get(r, url);
    if (!c && url2)
        c = cover_get(r, url2);
    if (c && c->px) {
        int dw = w, dh = c->h * w / c->w;
        if (dh > h) {
            dh = h;
            dw = c->w * h / c->h;
        }
        int dx = x + (w - dw) / 2, dy = y + (h - dh) / 2;
        gfx_image(s, dx, dy, dw, dh, c->px, c->w, c->h, c->w);
        gfx_fill(s, dx, dy, dw, 2, UI_LINE);
        gfx_fill(s, dx, dy + dh - 2, dw, 2, UI_LINE);
        gfx_fill(s, dx, dy, 2, dh, UI_LINE);
        gfx_fill(s, dx + dw - 2, dy, 2, dh, UI_LINE);
    } else {
        gfx_rrect(g, s, x, y, w, h, 8, UI_FAINT);
        int ic = w < 120 ? 40 : 80;
        gfx_icon(g, s, ICON_BOOK, x + (w - ic) / 2, y + (h - ic) / 2, ic, 0, UI_LINE);
    }
}

static void draw_entry(opds_page *r, pf_screen *s, int x, int y, int w, const opds_entry *e, int line)
{
    gfx *g = r->env.g;
    int pad = 32, right = x + w - pad, book = opds_entry_is_book(e);
    gfx_style ts = { FONT_REGULAR, 38, UI_BLACK }, ss = { FONT_REGULAR, 30, UI_TEXT2 };
    const char *title = *e->title ? e->title : "(без названия)";
    gfx_icon(g, s, ICON_NEXT, right - 36, y + FEED_ROW_H / 2 - 18, 36, 0, UI_TEXT3);
    right -= 56;
    if (book) {
        int cw = 90, ch = 120, cx = x + pad, cy = y + (FEED_ROW_H - ch) / 2;
        draw_thumb(r, s, cx, cy, cw, ch, opds_entry_cover(e, 1), NULL);
        int tx = cx + cw + 28;
        opds_format fm[MAX_FMT];
        int nf = opds_formats(e, fm, MAX_FMT);
        char fl[128] = "";
        for (int i = 0; i < nf; i++) {
            if (!known_fmt(&fm[i]))
                continue;
            if (*fl)
                strncat(fl, " · ", sizeof fl - strlen(fl) - 1);
            strncat(fl, fm[i].label, sizeof fl - strlen(fl) - 1);
        }
        int lines = gfx_text_wrap(g, NULL, 0, 0, right - tx, 42, 2, ts, title);
        if (lines < 2) {
            gfx_text_fit(g, s, tx, y + 54, right - tx, ts, title);
            if (*e->author)
                gfx_text_fit(g, s, tx, y + 94, right - tx, ss, e->author);
            if (*fl) {
                gfx_style fs = { FONT_REGULAR, 30, UI_TEXT3 };
                gfx_text_fit(g, s, tx, y + 134, right - tx, fs, fl);
            }
        } else {
            /* название в две строки: автор и форматы — в одной третьей */
            gfx_text_wrap(g, s, tx, y + 44, right - tx, 42, 2, ts, title);
            char l3[420] = "";
            snprintf(l3, sizeof l3, "%s%s%s", e->author, *e->author && *fl ? " · " : "", fl);
            if (*l3)
                gfx_text_fit(g, s, tx, y + 130, right - tx, ss, l3);
        }
    } else {
        int tx = x + pad + 56 + 28;
        gfx_icon(g, s, ICON_FOLDER, x + pad, y + (FEED_ROW_H - 56) / 2, 56, 0, UI_TEXT2);
        int lines = gfx_text_wrap(g, NULL, 0, 0, right - tx, 42, 2, ts, title);
        if (lines >= 2) {
            gfx_text_wrap(g, s, tx, y + 62, right - tx, 42, 2, ts, title);
        } else if (*e->summary) {
            gfx_text_fit(g, s, tx, y + 66, right - tx, ts, title);
            gfx_text_fit(g, s, tx, y + 110, right - tx, ss, e->summary);
        } else {
            gfx_text_fit(g, s, tx, y + FEED_ROW_H / 2 + 14, right - tx, ts, title);
        }
    }
    if (line)
        gfx_fill(s, x + pad, y + FEED_ROW_H - 2, w - pad, 2, UI_FAINT);
}

/* Круглая кнопка «‹» или «›» внизу. */
static ui_rect draw_pager_btn(opds_page *r, pf_screen *s, int x, int y, int w, int h, int next, int on)
{
    gfx *g = r->env.g;
    if (on)
        gfx_rrect_line(g, s, x, y, w, h, h / 2.0f, 3, UI_BLACK);
    else
        gfx_rrect_line(g, s, x, y, w, h, h / 2.0f, 3, UI_LINE);
    gfx_icon(g, s, next ? ICON_NEXT : ICON_BACK, x + (w - 52) / 2, y + (h - 52) / 2, 52, 0, on ? UI_BLACK : UI_LINE);
    return on ? (ui_rect){ x, y, w, h } : (ui_rect){ 0, 0, 0, 0 };
}

static void draw_feed(opds_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    frame *f = top(r);
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, y = ui_content_top(), bottom = ui_content_bottom(s);
    r->hits = ui_header(g, s, f ? f->title : "Каталог", 1, 0, 1);
    cover_next_gen(r);
    r->search = r->pg_prev = r->pg_next = r->rows_rect = r->remove_btn = (ui_rect){ 0, 0, 0, 0 };
    r->login_btn = r->logout_btn = (ui_rect){ 0, 0, 0, 0 };
    r->nrows = 0;
    if (!f || !f->feed)
        return;
    opds_feed *fd = f->feed;
    int idx = r->nframes - 1, per = frame_per(r, idx);
    r->has_search = search_source(r) != NULL;
    if (r->has_search) {
        /* в поле — текущий запрос; касание меняет его */
        r->search = (ui_rect){ x, y + 4, w, BTN_H };
        gfx_rrect(g, s, x, y + 4, w, BTN_H, BTN_H / 2.0f, UI_WHITE);
        gfx_icon(g, s, ICON_SEARCH, x + 36, y + 4 + 24, 52, 0, UI_TEXT2);
        if (*f->term) {
            gfx_style st = { FONT_REGULAR, 38, UI_BLACK };
            gfx_text_fit(g, s, x + 112, y + 4 + 64, w - 112 - 40, st, f->term);
        } else {
            gfx_style st = { FONT_REGULAR, 38, UI_TEXT3 };
            gfx_text(g, s, x + 112, y + 4 + 64, st, "Поиск в каталоге");
        }
    }
    int rows_y = y + (r->has_search ? 4 + BTN_H + 16 : 8);
    int pages = frame_npages(r, idx);
    if (f->page >= pages)
        f->page = pages - 1;
    int first = f->page * per, last = first + per < fd->nentries ? first + per : fd->nentries;
    if (fd->nentries == 0) {
        ui_card(g, s, x, rows_y, w, 200);
        gfx_style st = { FONT_REGULAR, 38, UI_TEXT2 };
        gfx_text_wrap_center(g, s, x + 40, rows_y + 88, w - 80, 52, 2, st, "Здесь ничего нет");
        r->rows_rect = (ui_rect){ x, rows_y, w, 200 };
    } else {
        ui_card(g, s, x, rows_y, w, (last - first) * FEED_ROW_H);
        for (int i = first; i < last; i++) {
            int ry = rows_y + (i - first) * FEED_ROW_H;
            draw_entry(r, s, x, ry, w, &fd->entries[i], i + 1 < last);
            r->rows[r->nrows] = (ui_rect){ x, ry, w, FEED_ROW_H };
            r->row_item[r->nrows++] = i;
        }
        r->rows_rect = (ui_rect){ x, rows_y, w, (last - first) * FEED_ROW_H };
    }
    /* листание «‹ 2 из 3 ›»: экраны этой ленты и страницы каталога на сервере — одной нумерацией;
     * на краю «›» грузит следующую страницу каталога, «‹» — предыдущую */
    int cur, total, more;
    frame_numbers(r, &cur, &total, &more);
    int more_next = f->page + 1 < pages || *fd->next || f->dir == -1;
    int more_prev = f->page > 0 || *fd->prev || f->dir == 1;
    int pager = total > 1 || more || *fd->prev || f->dir;
    int pager_y = bottom - PAGER_H - 16;
    if (pager) {
        int bw = 200;
        r->pg_prev = draw_pager_btn(r, s, x, pager_y, bw, PAGER_H, 0, more_prev);
        r->pg_next = draw_pager_btn(r, s, x + w - bw, pager_y, bw, PAGER_H, 1, more_next);
        char buf[96];
        snprintf(buf, sizeof buf, "%d из %d%s", cur, total, more ? "+" : "");
        gfx_style st = { FONT_REGULAR, 36, UI_TEXT2 };
        int tw = gfx_text_width(g, st, buf);
        gfx_text(g, s, x + (w - tw) / 2, pager_y + PAGER_H / 2 + 13, st, buf);
    }
    /* корень своего каталога: внизу — вход («Изменить вход», «Выйти») и убрать его из списка */
    if (is_own_root(r, idx) && f->page == pages - 1) {
        int by = (pager ? pager_y : bottom) - BTN_H - 16;
        ui_pill(g, s, x, by, w, BTN_H, "Убрать из моих каталогов", 0);
        r->remove_btn = (ui_rect){ x, by, w, BTN_H };
        by -= BTN_H + 16;
        if (*r->cat.login || *r->cat.pass) {
            int bw = (w - 16) / 2;
            ui_pill(g, s, x, by, bw, BTN_H, "Изменить вход", 0);
            ui_pill(g, s, x + bw + 16, by, w - bw - 16, BTN_H, "Выйти", 0);
            r->login_btn = (ui_rect){ x, by, bw, BTN_H };
            r->logout_btn = (ui_rect){ x + bw + 16, by, w - bw - 16, BTN_H };
        } else {
            ui_pill(g, s, x, by, w, BTN_H, "Изменить вход", 0);
            r->login_btn = (ui_rect){ x, by, w, BTN_H };
        }
    }
}

/* Выбранный формат и его подпись для кнопки. */
static void book_open(opds_page *r, const opds_entry *e)
{
    r->book = e;
    int n = opds_formats(e, r->fm, MAX_FMT), k = 0;
    for (int i = 0; i < n; i++) /* «Файл» без имени — только если других нет */
        if (known_fmt(&r->fm[i]))
            r->fm[k++] = r->fm[i];
    r->nfm = k ? k : n;
    r->fmt = 0;
    r->dl.state = DL_IDLE;
    r->dl.err[0] = 0;
    r->auth_tries = 0;
    r->view = V_BOOK;
}

static int zone_y(const pf_screen *s)
{
    return ui_content_bottom(s) - ACTION_H;
}

/* Ход скачивания: подпись и полоса (без кнопки) — их одних и обновляем при ходе. */
static ui_rect draw_dl_strip(opds_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, zone = zone_y(s);
    gfx_style ms = { FONT_MEDIUM, 36, UI_BLACK };
    char buf[300], a[40], b[40];
    pthread_mutex_lock(&r->mu);
    long long done = r->dl.done, total = r->dl.total;
    pthread_mutex_unlock(&r->mu);
    gfx_fill(s, 0, zone, s->width, 110, UI_BG);
    ui_format_size(a, sizeof a, done);
    if (total > 0) {
        ui_format_size(b, sizeof b, total);
        snprintf(buf, sizeof buf, "Загрузка: %s из %s", a, b);
    } else {
        snprintf(buf, sizeof buf, "Загрузка: %s", a);
    }
    gfx_text_fit(g, s, x + 8, zone + 50, w - 16, ms, buf);
    ui_progress(g, s, x, zone + 72, w, 28, total > 0 ? (float)((double)done / (double)total) : 0.0f);
    r->dl.shown_done = done;
    r->dl.shown_total = total;
    return (ui_rect){ 0, zone, s->width, 110 };
}

/* Зона внизу карточки книги: «Скачать» / ход / «Читать». */
static void draw_action(opds_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, zone = zone_y(s);
    gfx_fill(s, 0, zone, s->width, ACTION_H, UI_BG);
    gfx_style ts = { FONT_REGULAR, 34, UI_TEXT2 }, ms = { FONT_MEDIUM, 36, UI_BLACK };
    int bh = 110, by = zone + ACTION_H - bh - 16;
    r->btn1 = r->btn2 = r->dir_btn = (ui_rect){ 0, 0, 0, 0 };
    char buf[300], where[200];
    switch (r->dl.state) {
    case DL_RUN:
        draw_dl_strip(r, s);
        ui_pill(g, s, x, by, w, bh, "Отмена", 0);
        r->btn1 = (ui_rect){ x, by, w, bh };
        break;
    case DL_DONE:
        snprintf(buf, sizeof buf, "Скачано: %s", r->dl.name);
        gfx_text_fit_middle(g, s, x + 8, zone + 54, w - 16, ms, buf);
        gfx_text_fit(g, s, x + 8, zone + 104, w - 16, ts, "Книга уже в вашей библиотеке");
        ui_pill(g, s, x, by, w / 2 - 12, bh, "Читать", 1);
        ui_pill(g, s, x + w / 2 + 12, by, w / 2 - 12, bh, "К списку", 0);
        r->btn1 = (ui_rect){ x, by, w / 2 - 12, bh };
        r->btn2 = (ui_rect){ x + w / 2 + 12, by, w / 2 - 12, bh };
        break;
    case DL_FAIL:
        gfx_text_wrap(g, s, x + 8, zone + 54, w - 16, 46, 2, ms, r->dl.err);
        ui_pill(g, s, x, by, w, bh, "Повторить", 1);
        r->btn1 = (ui_rect){ x, by, w, bh };
        break;
    default:
        if (!r->nfm) {
            gfx_text_wrap(g, s, x + 8, zone + 54, w - 16, 46, 2, ms, "В каталоге нет файла, который можно скачать");
            break;
        }
        save_where(r, where, sizeof where);
        snprintf(buf, sizeof buf, "Сохранится в: %s", where);
        gfx_text_fit_middle(g, s, x + 8, zone + 60, w - 16 - 56, ts, buf);
        gfx_icon(g, s, ICON_NEXT, x + w - 48, zone + 36, 40, 0, UI_TEXT2);
        r->dir_btn = (ui_rect){ x, zone, w, BTN_H };
        if (known_fmt(&r->fm[r->fmt]))
            snprintf(buf, sizeof buf, "Скачать %s", r->fm[r->fmt].label);
        else
            snprintf(buf, sizeof buf, "Скачать");
        ui_pill(g, s, x, by, w, bh, buf, 1);
        r->btn1 = (ui_rect){ x, by, w, bh };
        break;
    }
}

static void draw_book(opds_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    const opds_entry *e = r->book;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, y = ui_content_top() + 8;
    r->hits = ui_header(g, s, "О книге", 1, 0, 1);
    cover_next_gen(r);
    memset(r->fmt_hit, 0, sizeof r->fmt_hit);
    if (!e)
        return;
    gfx_style cs = { FONT_MEDIUM, 34, UI_BLACK };
    /* форматы-пилюли: высота не меньше BTN_H, между ними 16; не влезли в ряд — вторым рядом */
    int pill_x[MAX_FMT], pill_w[MAX_FMT], pill_row[MAX_FMT], rows = 0;
    if (r->nfm > 1) {
        int px = x + 40, row = 0, right = x + w - 40;
        for (int i = 0; i < r->nfm; i++) {
            int pw = gfx_text_width(g, cs, r->fm[i].label) + 64;
            if (pw < 140)
                pw = 140;
            if (px + pw > right && px > x + 40) {
                row++;
                px = x + 40;
            }
            pill_x[i] = px;
            pill_w[i] = pw;
            pill_row[i] = row;
            px += pw + 16;
        }
        rows = row + 1;
    }
    int ch = 360, cw = 270;
    int pills_h = rows ? rows * BTN_H + (rows - 1) * 16 + 30 : 0;
    int card_h = 40 + ch + 30 + pills_h + (rows ? 30 : 0);
    if (!rows)
        card_h = 40 + ch + 40;
    ui_card(g, s, x, y, w, card_h);
    r->cover_rect = (ui_rect){ x + 40, y + 40, cw, ch };
    draw_thumb(r, s, x + 40, y + 40, cw, ch, opds_entry_cover(e, 0), opds_entry_cover(e, 1));
    int tx = x + 40 + cw + 40, tw = x + w - 40 - tx;
    gfx_style ts = { FONT_MEDIUM, 46, UI_BLACK }, as = { FONT_REGULAR, 34, UI_TEXT2 }, fs = { FONT_REGULAR, 32, UI_TEXT2 };
    int lines = gfx_text_wrap(g, s, tx, y + 40 + 46, tw, 58, 4, ts, *e->title ? e->title : "(без названия)");
    int ay = y + 40 + 46 + (lines - 1) * 58 + 54;
    if (*e->author)
        gfx_text_wrap(g, s, tx, ay, tw, 44, 3, as, e->author);
    if (r->nfm) {
        char sz[40], buf[96];
        ui_format_size(sz, sizeof sz, r->fm[r->fmt].link->length);
        const char *lab = known_fmt(&r->fm[r->fmt]) ? r->fm[r->fmt].label : "";
        snprintf(buf, sizeof buf, "%s%s%s", lab, *lab && *sz ? " · " : "", sz);
        if (*buf)
            gfx_text_fit(g, s, tx, y + 40 + ch - 6, tw, fs, buf);
    }
    for (int i = 0; i < r->nfm && rows; i++) {
        int py = y + 40 + ch + 30 + pill_row[i] * (BTN_H + 16);
        ui_pill(g, s, pill_x[i], py, pill_w[i], BTN_H, r->fm[i].label, i == r->fmt);
        r->fmt_hit[i] = (ui_rect){ pill_x[i], py, pill_w[i], BTN_H };
    }
    /* аннотация */
    int sy = y + card_h + 16;
    if (*e->summary) {
        ui_caption(g, s, x + 8, sy + 40, "Описание");
        gfx_style ss = { FONT_REGULAR, 34, UI_TEXT2 };
        int max_lines = (zone_y(s) - (sy + 100)) / 46;
        if (max_lines > 0)
            gfx_text_wrap(g, s, x + 8, sy + 96, w - 16, 46, max_lines, ss, e->summary);
    } else {
        gfx_style ss = { FONT_REGULAR, 34, UI_TEXT3 };
        gfx_text(g, s, x + 8, sy + 56, ss, "Описания нет");
    }
    draw_action(r, s);
}

/* Две кнопки диалога в ряд: ov1 слева, ov2 справа. */
static void dialog_buttons(opds_page *r, pf_screen *s, int x, int w, int y, const char *l1, int fill1, const char *l2, int fill2)
{
    int bw = (w - 96) / 2 - 12;
    ui_pill(r->env.g, s, x + 48, y, bw, 112, l1, fill1);
    ui_pill(r->env.g, s, x + w / 2 + 12, y, bw, 112, l2, fill2);
    r->ov1 = (ui_rect){ x + 48, y, bw, 112 };
    r->ov2 = (ui_rect){ x + w / 2 + 12, y, bw, 112 };
}

/* Диалоги (загрузка, вход, ошибка, подтверждение) — по центру на очищенном фоне: под ними ничего
 * от списка не остаётся. Фон и заголовок уже нарисованы. */
static void draw_overlay(opds_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    int top = ui_content_top(), mid = (top + ui_content_bottom(s)) / 2;
    gfx_style ts = { FONT_MEDIUM, 44, UI_BLACK }, ss = { FONT_REGULAR, 36, UI_TEXT2 };
    r->ov1 = r->ov2 = (ui_rect){ 0, 0, 0, 0 };
    char buf[400], host[300];
    if (r->ov == OV_LOADING) {
        int login = r->kb_next || r->kb_want;
        url_host(r->last.kind == J_SEARCH && !*r->last.url ? r->last.osd : r->last.url, host, sizeof host);
        gfx_icon(g, s, ICON_CATALOG, s->width / 2 - 56, mid - 230, 112, 0, UI_LINE);
        if (login) {
            gfx_text_wrap_center(g, s, x, mid - 60, w, 56, 2, ts, "Введите логин и пароль");
            snprintf(buf, sizeof buf, "для %s", host);
            gfx_text_wrap_center(g, s, x, mid + 4, w, 46, 1, ss, buf);
        } else {
            gfx_text_wrap_center(g, s, x, mid - 40, w, 56, 1, ts, r->net_wait ? "Подключаюсь к Wi‑Fi…" : host);
        }
        int bw = 600, by = mid + 70;
        ui_pill(g, s, (s->width - bw) / 2, by, bw, 112, "Отмена", 0);
        r->ov1 = (ui_rect){ (s->width - bw) / 2, by, bw, 112 };
        return;
    }
    int h = 520, y = mid - h / 2;
    ui_card(g, s, x, y, w, h);
    int by = y + h - 48 - 112;
    switch (r->ov) {
    case OV_ERROR:
        gfx_text(g, s, x + 48, y + 100, ts, "Не получилось");
        gfx_text_wrap(g, s, x + 48, y + 170, w - 96, 46, 4, ss, r->msg);
        if (r->can_retry) {
            dialog_buttons(r, s, x, w, by, "Закрыть", 0, r->net_choose ? "Выбрать сеть" : "Повторить", 1);
        } else {
            ui_pill(g, s, x + 48, by, w - 96, 112, "Закрыть", 0);
            r->ov1 = (ui_rect){ x + 48, by, w - 96, 112 };
        }
        break;
    case OV_STOP:
        gfx_text_wrap(g, s, x + 48, y + 100, w - 96, 56, 2, ts, "Остановить загрузку?");
        gfx_text_wrap(g, s, x + 48, y + 190, w - 96, 46, 3, ss, "Уже скачанное пропадёт — придётся начать заново.");
        dialog_buttons(r, s, x, w, by, "Остановить", 0, "Продолжить", 1);
        break;
    default: /* OV_CONFIRM */
        snprintf(buf, sizeof buf, "Убрать «%s» из моих каталогов?",
                 r->confirm_idx >= 0 && r->confirm_idx < r->nmine ? r->mine[r->confirm_idx].name : "");
        gfx_text_wrap(g, s, x + 48, y + 100, w - 96, 56, 2, ts, buf);
        gfx_text_wrap(g, s, x + 48, y + 250, w - 96, 46, 2, ss, "Сам каталог на сервере не изменится — пропадёт только запись здесь.");
        dialog_buttons(r, s, x, w, by, "Отмена", 0, "Убрать", 1);
        break;
    }
}

/* ---- Страница ---- */

static void opds_open(page *p)
{
    opds_page *r = (opds_page *)p;
    cats_load(r);
    r->view = V_LIST;
    r->ov = OV_NONE;
    r->stop_leave = 0;
    dirsel_load(r->ds);
    r->lpage = 0;
    r->net_ok = 0;
    r->net_wait = r->net_choose = 0;
    r->kb_next = r->kb_want = r->kb_got = 0;
    r->pend = 0;
    r->nohttp = net_http_ready(r->nohttp_why, sizeof r->nohttp_why) != 0;
    frames_clear(r);
}

static void opds_close(page *p)
{
    opds_page *r = (opds_page *)p;
    job_abandon(r);
    r->cov_abort = 1;
    cover_next_gen(r); /* заказанные, но не начатые обложки больше не нужны */
    r->dl.state = DL_IDLE;
    r->ov = OV_NONE;
    r->stop_leave = 0;
    dirsel_load(r->ds); /* закрывает выбор папки */
    frames_clear(r);
}

static void opds_draw(page *p, pf_screen *s)
{
    opds_page *r = (opds_page *)p;
    r->cov_abort = 0;
    fill_bg(r, s);
    if (dirsel_active(r->ds)) {
        dirsel_draw(r->ds, s);
        return;
    }
    if (r->ov) {
        /* диалог на чистом фоне: список под ним не рисуем */
        int login = r->kb_next || r->kb_want;
        const char *title = r->ov == OV_LOADING ? (login ? "Нужен вход" : "Загрузка…") : view_title(r);
        r->hits = ui_header(r->env.g, s, title, 1, 0, 1);
        draw_overlay(r, s);
        return;
    }
    switch (r->view) {
    case V_FEED: draw_feed(r, s); break;
    case V_BOOK: draw_book(r, s); break;
    default: draw_list(r, s); break;
    }
}

/* Лента наверху стека выгружена из памяти — загрузить заново. */
static int ensure_top(opds_page *r)
{
    frame *f = top(r);
    if (f && !f->feed)
        return fetch_feed(r, P_REFRESH, f->url);
    return PAGE_REDRAW;
}

static int go_back(opds_page *r)
{
    if (r->view == V_BOOK) {
        if (r->dl.state == DL_RUN) { /* идёт скачивание — спросить */
            r->ov = OV_STOP;
            r->stop_leave = 0;
            return PAGE_REDRAW;
        }
        r->dl.state = DL_IDLE;
        r->view = V_FEED;
        return PAGE_REDRAW;
    }
    if (r->view == V_FEED) {
        job_abandon(r);
        while (r->nframes && top(r)->dir != 0)
            frame_pop(r);
        frame_pop(r);
        if (!r->nframes) {
            r->view = V_LIST;
            return PAGE_REDRAW;
        }
        return ensure_top(r);
    }
    return PAGE_CLOSE;
}

/* Листание ленты: сначала по экранам, потом по страницам каталога. */
static int feed_turn(opds_page *r, int dir)
{
    frame *f = top(r);
    if (!f || !f->feed)
        return 0;
    opds_feed *fd = f->feed;
    int pages = frame_npages(r, r->nframes - 1);
    if (dir > 0) {
        if (f->page + 1 < pages) {
            f->page++;
            return PAGE_REDRAW;
        }
        if (f->dir == -1) { /* пришли «назад» — страница дальше уже в памяти */
            frame_pop(r);
            return ensure_top(r);
        }
        if (*fd->next)
            return fetch_feed(r, P_NEXT, fd->next);
    } else {
        if (f->page > 0) {
            f->page--;
            return PAGE_REDRAW;
        }
        if (f->dir == 1) {
            frame_pop(r);
            return ensure_top(r);
        }
        if (*fd->prev)
            return fetch_feed(r, P_PREV, fd->prev);
    }
    return 0;
}

static void flash(opds_page *r, ui_rect rc)
{
    pf_screen *s = pf_get_screen();
    if (s)
        ui_flash(r->env.g, s, rc);
}

static int overlay_tap(opds_page *r, int x, int y)
{
    if (ui_inside(r->hits.back, x, y)) { /* «‹» в заголовке — как безопасная кнопка диалога */
        ui_rect t = r->ov == OV_STOP ? r->ov2 : r->ov1;
        x = t.x + t.w / 2;
        y = t.y + t.h / 2;
    }
    if (r->ov == OV_LOADING) {
        if (ui_inside(r->ov1, x, y)) {
            r->net_wait = 0;
            job_abandon(r);
            r->kb_next = r->kb_want = 0;
            r->ov = OV_NONE;
            return PAGE_REDRAW;
        }
        return 0;
    }
    if (r->ov == OV_ERROR) {
        if (r->can_retry && ui_inside(r->ov2, x, y)) {
            flash(r, r->ov2);
            r->ov = OV_NONE;
            job_spec sp = r->last;
            return start_job(r, &sp);
        }
        if (ui_inside(r->ov1, x, y)) {
            r->ov = OV_NONE;
            r->net_choose = 0;
            return PAGE_REDRAW;
        }
        return 0;
    }
    if (r->ov == OV_STOP) {
        if (ui_inside(r->ov1, x, y)) { /* остановить */
            job_abandon(r);
            r->dl.state = DL_IDLE;
            r->ov = OV_NONE;
            if (r->stop_leave) {
                r->stop_leave = 0;
                return PAGE_CLOSE;
            }
            r->view = V_FEED;
            return PAGE_REDRAW;
        }
        if (ui_inside(r->ov2, x, y)) { /* продолжить */
            r->ov = OV_NONE;
            r->stop_leave = 0;
            return PAGE_REDRAW;
        }
        return 0;
    }
    if (ui_inside(r->ov2, x, y)) { /* убрать каталог из списка */
        int i = r->confirm_idx;
        if (i >= 0 && i < r->nmine) {
            memmove(&r->mine[i], &r->mine[i + 1], (size_t)(r->nmine - i - 1) * sizeof r->mine[0]);
            r->nmine--;
            cats_save(r);
        }
        job_abandon(r);
        frames_clear(r);
        r->cat_idx = -1;
        r->view = V_LIST;
        r->ov = OV_NONE;
        return PAGE_REDRAW;
    }
    if (ui_inside(r->ov1, x, y)) {
        r->ov = OV_NONE;
        return PAGE_REDRAW;
    }
    return 0;
}

static int start_download(opds_page *r)
{
    if (!r->book || !r->nfm)
        return 0;
    const opds_format *fm = &r->fm[r->fmt];
    job_spec sp;
    memset(&sp, 0, sizeof sp);
    sp.kind = J_DOWNLOAD;
    sp.purpose = P_DOWNLOAD;
    scpy(sp.url, sizeof sp.url, fm->link->href);
    dirsel_dir(r->ds, sp.dir, sizeof sp.dir);
    scpy(sp.ext, sizeof sp.ext, fm->ext);
    if (*r->book->author)
        snprintf(sp.name, sizeof sp.name, "%.90s — %.110s", r->book->author, r->book->title);
    else
        snprintf(sp.name, sizeof sp.name, "%.200s", r->book->title);
    for (char *c = sp.name; *c; c++) /* «/» в названии — не папка */
        if (*c == '/' || *c == '\\')
            *c = '-';
    spec_auth(r, &sp, sp.url);
    r->auth_tries = 0;
    return start_job(r, &sp);
}

static int opds_tap(page *p, int x, int y)
{
    opds_page *r = (opds_page *)p;
    if (dirsel_active(r->ds))
        return dirsel_tap(r->ds, x, y) == DS_NONE ? 0 : PAGE_REDRAW;
    if (r->ov)
        return overlay_tap(r, x, y);
    if (ui_inside(r->hits.back, x, y))
        return go_back(r);
    if (r->view == V_LIST) {
        if (ui_inside(r->hits.prev, x, y) && r->lpage > 0) {
            r->lpage--;
            return PAGE_REDRAW;
        }
        if (ui_inside(r->hits.next, x, y)) {
            r->lpage++;
            return PAGE_REDRAW;
        }
        for (int i = 0; i < r->nrows; i++) {
            if (!ui_inside(r->rows[i], x, y))
                continue;
            int k = r->row_item[i];
            if (k == r->nmine)
                return ask(r, KB_ADDR, "Адрес каталога OPDS", "http://", 400);
            flash(r, r->rows[i]);
            if (k < r->nmine)
                return open_catalog(r, &r->mine[k], k);
            return open_catalog(r, &presets[k - r->nmine - 1], -1);
        }
        return 0;
    }
    if (r->view == V_FEED) {
        frame *f = top(r);
        if (!f || !f->feed)
            return 0;
        if (ui_inside(r->search, x, y))
            return ask(r, KB_SEARCH, "Поиск в каталоге", f->term, 200);
        if (ui_inside(r->login_btn, x, y)) {
            flash(r, r->login_btn);
            return ask(r, KB_ELOGIN, "Логин", r->cat.login, 90);
        }
        if (ui_inside(r->logout_btn, x, y)) {
            flash(r, r->logout_btn);
            set_login(r, "", "");
            return PAGE_REDRAW;
        }
        if (ui_inside(r->remove_btn, x, y)) {
            r->confirm_idx = r->cat_idx;
            r->ov = OV_CONFIRM;
            return PAGE_REDRAW;
        }
        if (ui_inside(r->pg_prev, x, y)) {
            if (f->page == 0 && f->dir != 1 && *f->feed->prev) /* страница с сервера — дольше */
                flash(r, r->pg_prev);
            return feed_turn(r, -1);
        }
        if (ui_inside(r->pg_next, x, y)) {
            if (f->page + 1 >= frame_npages(r, r->nframes - 1) && f->dir != -1 && *f->feed->next)
                flash(r, r->pg_next);
            return feed_turn(r, 1);
        }
        for (int i = 0; i < r->nrows; i++) {
            if (!ui_inside(r->rows[i], x, y))
                continue;
            const opds_entry *e = &f->feed->entries[r->row_item[i]];
            if (opds_entry_is_book(e)) {
                book_open(r, e);
                return PAGE_REDRAW;
            }
            const opds_link *nav = opds_entry_nav(e);
            if (nav) {
                flash(r, r->rows[i]);
                return fetch_feed(r, P_ENTER, nav->href);
            }
            return 0;
        }
        return 0;
    }
    /* V_BOOK */
    if (r->dir_btn.w && ui_inside(r->dir_btn, x, y) && r->dl.state != DL_RUN) {
        flash(r, r->dir_btn);
        dirsel_start(r->ds);
        return PAGE_REDRAW;
    }
    for (int i = 0; i < r->nfm && r->nfm > 1; i++)
        if (ui_inside(r->fmt_hit[i], x, y) && r->dl.state != DL_RUN) {
            r->fmt = i;
            r->dl.state = DL_IDLE;
            return PAGE_REDRAW;
        }
    if (ui_inside(r->btn1, x, y)) {
        switch (r->dl.state) {
        case DL_RUN:
            job_abandon(r);
            r->dl.state = DL_IDLE;
            return PAGE_REDRAW;
        case DL_DONE:
            flash(r, r->btn1);
            return PAGE_OPEN;
        default:
            flash(r, r->btn1);
            return start_download(r);
        }
    }
    if (ui_inside(r->btn2, x, y) && r->dl.state == DL_DONE)
        return go_back(r);
    return 0;
}

static int opds_key(page *p, pf_key key)
{
    opds_page *r = (opds_page *)p;
    if (r->ov || dirsel_active(r->ds) || (key != PF_KEY_PREV && key != PF_KEY_NEXT))
        return 0;
    if (r->view == V_LIST) {
        int pages = (list_count(r) + LIST_PER_PAGE - 1) / LIST_PER_PAGE;
        int np = r->lpage + (key == PF_KEY_NEXT ? 1 : -1);
        if (np < 0 || np >= pages)
            return 0;
        r->lpage = np;
        return PAGE_REDRAW;
    }
    if (r->view == V_FEED)
        return feed_turn(r, key == PF_KEY_NEXT ? 1 : -1);
    return 0;
}

/* Показать часть экрана частичным обновлением (после рисования в буфер). */
static void partial(ui_rect rc)
{
    pf_update(rc.x, rc.y, rc.w, rc.h, PF_UPDATE_PARTIAL);
}

static int opds_tick(page *p, pf_screen *s)
{
    opds_page *r = (opds_page *)p;
    int flags = r->pend;
    r->pend = 0;
    if (r->net_wait) {
        int rc = pf_net_poll();
        if (rc == 0)
            return flags;
        r->net_wait = 0;
        r->ov = OV_NONE;
        if (rc < 0) {
            set_error(r, "Известной сети Wi‑Fi рядом нет. Выберите сеть", 1);
            r->net_choose = 1;
            return flags | PAGE_REDRAW;
        }
        r->net_ok = 1;
        job_spec sp = r->last;
        return flags | start_job(r, &sp);
    }
    if (r->kb_next) {
        int what = r->kb_next;
        r->kb_next = 0;
        if (what == KB_LOGIN)
            flags |= ask(r, KB_LOGIN, "Логин", r->cat.login, 90);
        else if (what == KB_ELOGIN)
            flags |= ask(r, KB_ELOGIN, "Логин", r->cat.login, 90);
        else if (what == KB_EPASS)
            flags |= ask(r, KB_EPASS, "Пароль", "", 90);
        else
            flags |= ask(r, KB_PASS, "Пароль", "", 90);
    }
    job *done = NULL;
    pthread_mutex_lock(&r->mu);
    if (r->job && r->job->done) {
        done = r->job;
        r->job = NULL;
    }
    int newcov = r->cov_new;
    r->cov_new = 0;
    pthread_mutex_unlock(&r->mu);
    if (done) {
        flags |= job_finished(r, done);
        job_free(done);
        return flags | PAGE_REDRAW;
    }
    if (r->job || r->dl.state == DL_RUN)
        pf_keep_awake(60);
    if (flags)
        return flags;
    if (r->ov || dirsel_active(r->ds))
        return 0;
    double now = clock_ms();
    if (r->view == V_BOOK && r->dl.state == DL_RUN) {
        /* ход — частичным обновлением одной полосы: раз в PROGRESS_MS или когда она сдвинулась на 5 % */
        pthread_mutex_lock(&r->mu);
        long long d = r->dl.done, t = r->dl.total;
        pthread_mutex_unlock(&r->mu);
        double since = now - r->dl.last_draw;
        double was = r->dl.shown_total > 0 ? (double)r->dl.shown_done / (double)r->dl.shown_total : 0;
        double cur = t > 0 ? (double)d / (double)t : 0;
        int due = d != r->dl.shown_done && (since >= PROGRESS_MS || (t > 0 && since >= 1000 && cur - was >= PROGRESS_STEP));
        if (due) {
            r->dl.last_draw = now;
            partial(draw_dl_strip(r, s));
        }
    }
    if (newcov && now - r->cover_draw >= COVER_MS) {
        /* пришла обложка: перерисовать только то, где они бывают */
        r->cover_draw = now;
        if (r->view == V_FEED && r->rows_rect.w) {
            frame *f = top(r);
            if (f && f->feed) {
                ui_rect rc = r->rows_rect;
                int per = frame_per(r, r->nframes - 1);
                int first = f->page * per;
                int last = first + per < f->feed->nentries ? first + per : f->feed->nentries;
                r->nrows = 0;
                ui_card(r->env.g, s, rc.x, rc.y, rc.w, rc.h);
                for (int i = first; i < last; i++) {
                    int ry = rc.y + (i - first) * FEED_ROW_H;
                    draw_entry(r, s, rc.x, ry, rc.w, &f->feed->entries[i], i + 1 < last);
                    r->rows[r->nrows] = (ui_rect){ rc.x, ry, rc.w, FEED_ROW_H };
                    r->row_item[r->nrows++] = i;
                }
                partial(rc);
            }
        } else if (r->view == V_BOOK && r->book) {
            ui_rect rc = r->cover_rect;
            ui_card(r->env.g, s, rc.x - 8, rc.y - 8, rc.w + 16, rc.h + 16);
            draw_thumb(r, s, rc.x, rc.y, rc.w, rc.h, opds_entry_cover(r->book, 0), opds_entry_cover(r->book, 1));
            partial((ui_rect){ rc.x - 8, rc.y - 8, rc.w + 16, rc.h + 16 });
        }
    } else if (newcov) {
        pthread_mutex_lock(&r->mu);
        r->cov_new = 1; /* ещё рано — на следующий раз */
        pthread_mutex_unlock(&r->mu);
    }
    return 0;
}

static const char *opds_selected(page *p)
{
    opds_page *r = (opds_page *)p;
    return r->dl.state == DL_DONE ? r->dl.path : NULL;
}

static void opds_free(page *p)
{
    opds_page *r = (opds_page *)p;
    job_abandon(r);
    r->cov_abort = 1;
    pthread_mutex_lock(&r->mu);
    r->cov_quit = 1;
    pthread_cond_broadcast(&r->cov_cv);
    while (r->running > 0)
        pthread_cond_wait(&r->cv, &r->mu);
    pthread_mutex_unlock(&r->mu);
    frames_clear(r);
    for (int i = 0; i < COVERS; i++) {
        free(r->covs[i].url);
        free(r->covs[i].px);
    }
    pthread_mutex_destroy(&r->mu);
    pthread_cond_destroy(&r->cv);
    pthread_cond_destroy(&r->cov_cv);
    dirsel_free(r->ds);
    free(r);
}

/* Хаб хочет уйти со страницы: идёт скачивание книги — спросить «Остановить?». */
static int opds_leave(page *p)
{
    opds_page *r = (opds_page *)p;
    if (r->dl.state != DL_RUN)
        return 0;
    r->ov = OV_STOP;
    r->stop_leave = 1;
    return 1;
}

static const page_ops ops = { opds_open, opds_close, opds_draw, opds_tap, opds_key, opds_tick, opds_selected, opds_free, opds_leave };

page *opds_page_new(const page_env *env)
{
    opds_page *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->base.ops = &ops;
    r->base.gray = 1;
    r->env = *env;
    r->cat_idx = -1;
    snprintf(r->file, sizeof r->file, "%s/opds.txt", env->data_dir);
    char dir_file[1200];
    snprintf(dir_file, sizeof dir_file, "%s/opds_dir.txt", env->data_dir);
    r->ds = dirsel_new(env->g, env->library_root, env->data_dir, env->download_dir, dir_file, env->redraw, env->user);
    if (!r->ds) {
        free(r);
        return NULL;
    }
    pthread_mutex_init(&r->mu, NULL);
    pthread_cond_init(&r->cv, NULL);
    pthread_cond_init(&r->cov_cv, NULL);
    return &r->base;
}
