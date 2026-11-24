/* Синхронизация: папка на сервере (WebDAV или FTP). Книги — с сервера на книгу, места чтения и
 * закладки — в обе стороны (src/net/syncjob.h). Сеть — в потоке задачи, слияние мест чтения —
 * здесь, в tick, когда поток скачал серверные копии.
 *
 * Настройки — <папка данных>/sync.txt (ключ=значение; логин и пароль зашифрованы, src/base/cred.h).
 */
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../base/clock.h"
#include "../base/config.h"
#include "../base/cred.h"
#include "../base/log.h"
#include "../base/util.h"
#include "../net/net.h"
#include "../net/syncjob.h"
#include "page.h"
#include "widgets.h"

#define MAX_LINES 6
#define CARD_Y 550
#define PROG_H 420    /* карточка хода */
#define CARD_MAX 600  /* карточка итога не выше */
#define ROW_H UI_ROW_H
#define PAINT_MS 2000 /* ход передачи — на экране не чаще (если не сдвинулось на ≥ 5 %) */

enum { V_MAIN, V_SETUP };
enum { F_URL, F_LOGIN, F_PASSWORD, F_DIR, F_COUNT };

typedef struct {
    remote_cfg cfg;
    char msg[320];
    int ok;
    volatile int done;
    pthread_t th;
    int started;
} check;

/* Итог одной синхронизации: когда и что написать. */
typedef struct {
    long long time; /* 0 — не было */
    int cancelled;  /* остановлена вручную (для неудачной попытки) */
    char line[MAX_LINES][256];
    int n;
} block;

typedef struct {
    char title[96], file[256], counts[160], total[96];
    float frac;
} progress_view;

typedef struct sync_page sync_page;

typedef struct {
    sync_page *page;
    int field;
} kbd_ctx;

struct sync_page {
    page base;
    page_env env;
    ui_header_hits hits;
    char cfg_path[1100];
    char url[1024], login[128], password[128], dir[1024];
    int lost; /* логин/пароль из файла не расшифровались: бит 1 — логин, 2 — пароль; ввести заново */
    block last; /* последняя удачная синхронизация */
    block att;  /* последняя неудачная или остановленная попытка (после неё удачной не было) */
    int view;
    char msg[320]; /* что не получилось в начале (нет сети, нет FTP…) */
    ui_rect r_sync, r_edit, r_check, r_done, r_yes, r_no, rows[F_COUNT];
    int net_wait;   /* идёт подключение к Wi-Fi (pf_net_poll в tick) */
    int net_for;    /* что потом: NET_FOR_CHECK или NET_FOR_SYNC */
    int net_choose; /* известной сети рядом нет: следующее нажатие — окно выбора сети */
    int confirm; /* вопрос «Остановить синхронизацию?» */
    int card_h;  /* высота карточки итога/хода на главном экране */
    progress_view shown; /* что нарисовано в карточке хода */
    kbd_ctx kbd[F_COUNT];
    int open; /* страница на экране */

    sync_job *job;
    int merge_done, cancelling;
    notes_result nres;
    int merge_failed;
    double last_paint, last_awake;

    check chk;
    int chk_shown; /* результат проверки показывается */
};

/* ---- Настройки ---- */

static void default_dir(const sync_page *r, char *out, size_t len)
{
    snprintf(out, len, "%s/Sync", r->env.library_root);
}

static void local_dir(const sync_page *r, char *out, size_t len)
{
    if (!r->dir[0])
        default_dir(r, out, len);
    else if (r->dir[0] == '/')
        snprintf(out, len, "%s", r->dir);
    else
        snprintf(out, len, "%s/%s", r->env.library_root, r->dir);
    size_t n = strlen(out);
    while (n > 1 && out[n - 1] == '/')
        out[--n] = 0;
}

/* Весь sync.txt за один раз (запись с fsync — не по строке на каждое поле). */
static void save_all(const sync_page *r)
{
    FILE *f = file_save_begin(r->cfg_path);
    if (!f) {
        log_msg("синхронизация: не записать %s", r->cfg_path);
        return;
    }
    fprintf(f, "# Синхронизация с сервером. Логин и пароль зашифрованы.\n");
    fprintf(f, "url=%s\n", r->url);
    cred_config_put(f, "login", r->login);
    cred_config_put(f, "password", r->password);
    fprintf(f, "dir=%s\n", r->dir);
    if (r->lost)
        fprintf(f, "cred_lost=%d\n", r->lost);
    if (r->last.time) {
        fprintf(f, "last_time=%lld\n", r->last.time);
        for (int i = 0; i < r->last.n; i++)
            fprintf(f, "r%d=%s\n", i + 1, r->last.line[i]);
    }
    if (r->att.time) {
        fprintf(f, "att_time=%lld\natt_cancelled=%d\n", r->att.time, r->att.cancelled);
        for (int i = 0; i < r->att.n; i++)
            fprintf(f, "a%d=%s\n", i + 1, r->att.line[i]);
    }
    file_save_end(f, r->cfg_path, 1);
}

static void load_settings(sync_page *r)
{
    config c;
    config_load(&c, r->cfg_path);
    snprintf(r->url, sizeof r->url, "%s", config_get(&c, "url", ""));
    int a = cred_config_get(&c, "login", r->login, sizeof r->login);
    int b = cred_config_get(&c, "password", r->password, sizeof r->password);
    r->lost = (a == CRED_BAD ? 1 : 0) | (b == CRED_BAD ? 2 : 0) | (atoi(config_get(&c, "cred_lost", "0")) & 3);
    if (r->lost) /* пара — целиком: половинка бесполезна */
        r->login[0] = r->password[0] = 0;
    snprintf(r->dir, sizeof r->dir, "%s", config_get(&c, "dir", ""));
    block *bl[2] = { &r->last, &r->att };
    static const char *const tkey[2] = { "last_time", "att_time" }, *const lkey[2] = { "r%d", "a%d" };
    for (int b = 0; b < 2; b++) {
        bl[b]->time = atoll(config_get(&c, tkey[b], "0"));
        if (!bl[b]->time)
            continue;
        for (int i = 0; i < MAX_LINES; i++) {
            char k[8];
            snprintf(k, sizeof k, lkey[b], i + 1);
            const char *v = config_get(&c, k, "");
            if (*v)
                snprintf(bl[b]->line[bl[b]->n++], sizeof bl[b]->line[0], "%s", v);
        }
    }
    r->att.cancelled = atoi(config_get(&c, "att_cancelled", "0"));
    if (a == CRED_OLD || b == CRED_OLD) /* старое открытое — сразу зашифрованным */
        save_all(r);
}

/* Логин или пароль не расшифровались (другое устройство, порча файла): подсказка, что делать. 0 — всё в порядке. */
static int lost_hint(const sync_page *r, char *out, size_t len)
{
    if (!r->lost)
        return 0;
    snprintf(out, len, "%s не удалось прочитать. Нажмите «Изменить сервер» и введите заново.",
             r->lost == 3 ? "Логин и пароль" : r->lost == 1 ? "Логин" : "Пароль");
    return 1;
}

static void remote_of(const sync_page *r, remote_cfg *c)
{
    memset(c, 0, sizeof *c);
    snprintf(c->url, sizeof c->url, "%s", r->url);
    snprintf(c->login, sizeof c->login, "%s", r->login);
    snprintf(c->password, sizeof c->password, "%s", r->password);
}

/* Прежние сведения о скачанном относились к другому серверу — забываем. */
static void forget_state(const sync_page *r)
{
    static const char *const names[] = { "sync-books.txt", "sync-notes.txt" };
    for (int i = 0; i < 2; i++) {
        char p[1200];
        snprintf(p, sizeof p, "%s/%s", r->env.data_dir, names[i]);
        remove(p);
    }
}

/* ---- Проверка сервера ---- */

static void *check_run(void *arg)
{
    check *c = arg;
    char why[160];
    if (remote_is_ftp(c->cfg.url) && !net_has_protocol("ftp")) {
        snprintf(c->msg, sizeof c->msg, "В прошивке нет FTP — укажите адрес облачной папки (WebDAV): https://…");
    } else if (net_http_ready(why, sizeof why) != 0) {
        snprintf(c->msg, sizeof c->msg, "%s", why);
    } else {
        remote_entries e = { NULL, 0, 0 };
        char err[256];
        if (remote_list(&c->cfg, "", &e, NULL, err, sizeof err, NULL) != 0) {
            snprintf(c->msg, sizeof c->msg, "%s", err);
        } else {
            int books = 0, dirs = 0;
            for (int i = 0; i < e.count; i++) {
                if (e.items[i].is_dir)
                    dirs += e.items[i].name[0] != '.';
                else
                    books += sync_is_book(e.items[i].name);
            }
            c->ok = 1;
            if (!books && !dirs)
                snprintf(c->msg, sizeof c->msg, "Сервер отвечает, но папка пуста — положите в неё книги.");
            else
                snprintf(c->msg, sizeof c->msg, "Всё работает. В папке: книг — %d, папок — %d.", books, dirs);
        }
        remote_entries_free(&e);
    }
    c->done = 1;
    return NULL;
}

static void check_join(sync_page *r)
{
    if (r->chk.started) {
        pthread_join(r->chk.th, NULL);
        r->chk.started = 0;
    }
}

/* ---- Итог ---- */

static void add_line(block *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void add_line(block *b, const char *fmt, ...)
{
    if (b->n >= MAX_LINES)
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b->line[b->n++], sizeof b->line[0], fmt, ap);
    va_end(ap);
}

/* Удачная — в «последнюю удачную» (неудачная попытка забывается); не вышло или остановлена —
 * в «попытку», а время и итог последней удачной остаются как были. */
static void summarize(sync_page *r, const sync_status *s)
{
    block nb;
    memset(&nb, 0, sizeof nb);
    nb.time = (long long)time(NULL);
    block *b = &nb;
    if (s->error[0]) {
        add_line(b, "Не вышло: %s", s->error);
        if (s->n_new + s->n_updated)
            add_line(b, "Успели скачать: %d", s->n_new + s->n_updated);
        r->att = nb;
        save_all(r);
        return;
    }
    int got = s->n_new + s->n_updated;
    if (s->cancelled) {
        nb.cancelled = 1;
        add_line(b, got ? "Успели скачать: %d" : "Ничего не скачано", got);
    } else if (got == 0) {
        add_line(b, "Новых книг нет: всё уже на устройстве");
    } else if (s->n_updated) {
        add_line(b, "Скачано: %d %s, обновлено: %d", s->n_new, ui_plural(s->n_new, "книга", "книги", "книг"),
                 s->n_updated);
    } else {
        add_line(b, "Скачано: %d %s", s->n_new, ui_plural(s->n_new, "книга", "книги", "книг"));
    }
    if (!s->cancelled) {
        if (s->notes_error[0]) {
            add_line(b, "Места чтения не сведены: %s", s->notes_error);
        } else if (r->merge_done) {
            const notes_result *n = &r->nres;
            if (n->pos_updated + n->pos_imported + n->bm_updated)
                add_line(b, "Места чтения и закладки сведены с сервером; на устройстве обновлено: %d",
                         n->pos_updated + n->pos_imported + n->bm_updated);
            else
                add_line(b, "Места чтения и закладки сведены с сервером");
        }
    }
    if (s->n_server_deleted)
        add_line(b, "На сервере удалены: %d, на устройстве оставлены", s->n_server_deleted);
    if (s->n_failed)
        add_line(b, "Не скачано: %d — %s", s->n_failed, s->first_fail);
    if (s->n_nospace)
        add_line(b, "Не хватило места на устройстве: %d", s->n_nospace);
    if (s->n_conflict)
        add_line(b, "Изменены на устройстве, не тронуты: %d", s->n_conflict);
    if (s->n_dirs_failed)
        add_line(b, "Папок на сервере не открылось: %d", s->n_dirs_failed);
    if (s->cancelled) {
        r->att = nb;
    } else {
        r->last = nb;
        memset(&r->att, 0, sizeof r->att);
    }
    save_all(r);
}

/* ---- Рисование ---- */

static void format_time(long long t, char *out, size_t len)
{
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    snprintf(out, len, "%02d.%02d.%04d %d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour,
             tm.tm_min);
}

static void bytes_text(long long b, char *out, size_t len)
{
    ui_format_size(out, len, b);
}

/* Строка состояния задачи (для сравнения «изменилось ли что показывать») и подписи. */
static void make_view(const sync_page *r, const sync_status *s, progress_view *v)
{
    memset(v, 0, sizeof *v);
    v->frac = -1;
    if (r->cancelling) {
        snprintf(v->title, sizeof v->title, "Останавливаем…");
        return;
    }
    switch (s->stage) {
    case SJ_LIST:
        snprintf(v->title, sizeof v->title, "Смотрим, что на сервере");
        snprintf(v->counts, sizeof v->counts, "Папок: %d · книг: %d", s->dirs, s->files);
        break;
    case SJ_BOOKS: {
        if (!s->todo) {
            snprintf(v->title, sizeof v->title, "Новых книг нет");
            break;
        }
        snprintf(v->title, sizeof v->title, "Скачиваем книги");
        snprintf(v->file, sizeof v->file, "%s", s->cur);
        char a[32], b[32], c[32], d[32];
        bytes_text(s->cur_done, a, sizeof a);
        bytes_text(s->cur_total, b, sizeof b);
        snprintf(v->counts, sizeof v->counts, "Книга %d из %d · %s%s%s", s->done + 1 > s->todo ? s->todo : s->done + 1,
                 s->todo, a, s->cur_total > 0 ? " из " : "", s->cur_total > 0 ? b : "");
        long long all_done = s->bytes_done + s->cur_done;
        bytes_text(all_done, c, sizeof c);
        bytes_text(s->bytes_total, d, sizeof d);
        if (s->bytes_total > 0) {
            snprintf(v->total, sizeof v->total, "Всего: %s из %s", c, d);
            v->frac = (float)all_done / (float)s->bytes_total;
        } else {
            v->frac = (float)s->done / (float)s->todo;
        }
        if (v->frac > 1)
            v->frac = 1;
        break;
    }
    case SJ_NOTES:
        snprintf(v->title, sizeof v->title, "Места чтения и закладки");
        snprintf(v->counts, sizeof v->counts, "Берём с сервера…");
        break;
    case SJ_MERGE:
        snprintf(v->title, sizeof v->title, "Места чтения и закладки");
        snprintf(v->counts, sizeof v->counts, "Сводим с серверными…");
        break;
    default:
        snprintf(v->title, sizeof v->title, "Места чтения и закладки");
        snprintf(v->counts, sizeof v->counts, "Отправляем на сервер…");
        break;
    }
}

/* Строки карточки хода: у каждой своя область, чтобы обновлять только изменившееся. */
enum { PR_FILE, PR_COUNTS, PR_BAR, PR_TOTAL, PR_COUNT };

typedef struct {
    int y[PR_COUNT]; /* baseline (у полосы — верх); -1 — строки нет */
} prog_pos;

static void prog_layout(const progress_view *v, prog_pos *p)
{
    int y = CARD_Y + 160;
    for (int i = 0; i < PR_COUNT; i++)
        p->y[i] = -1;
    if (v->file[0]) {
        p->y[PR_FILE] = y;
        y += 56;
    }
    if (v->counts[0]) {
        p->y[PR_COUNTS] = y;
        y += 70;
    }
    if (v->frac >= 0) {
        p->y[PR_BAR] = y;
        y += 24 + 52;
        p->y[PR_TOTAL] = y;
    }
}

/* Нарисовать строку row (старое на белом стирается) и вернуть её область. */
static ui_rect prog_row(sync_page *r, pf_screen *s, const progress_view *v, const prog_pos *p, int row)
{
    gfx *g = r->env.g;
    int tx = UI_MARGIN + 40, tw = s->width - 2 * UI_MARGIN - 80;
    static const gfx_style ts = { FONT_REGULAR, 36, UI_BLACK }, cs = { FONT_REGULAR, 34, UI_TEXT2 };
    int y = p->y[row];
    ui_rect a = { tx - 8, 0, tw + 16, 0 };
    if (y < 0)
        return a;
    switch (row) {
    case PR_FILE:
        a.y = y - 42, a.h = 62;
        gfx_fill(s, a.x, a.y, a.w, a.h, UI_WHITE);
        gfx_text_fit_middle(g, s, tx, y, tw, ts, v->file);
        break;
    case PR_COUNTS:
        a.y = y - 40, a.h = 58;
        gfx_fill(s, a.x, a.y, a.w, a.h, UI_WHITE);
        gfx_text_fit(g, s, tx, y, tw, cs, v->counts);
        break;
    case PR_BAR:
        a.y = y - 6, a.h = 36;
        gfx_fill(s, a.x, a.y, a.w, a.h, UI_WHITE);
        ui_progress(g, s, tx, y, tw, 24, v->frac);
        break;
    default:
        a.y = y - 40, a.h = 58;
        gfx_fill(s, a.x, a.y, a.w, a.h, UI_WHITE);
        gfx_text_fit(g, s, tx, y, tw, cs, v->total);
        break;
    }
    return a;
}

static void draw_progress_card(sync_page *r, pf_screen *s, const progress_view *v)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    gfx_fill(s, x - 8, CARD_Y - 4, w + 16, PROG_H + 8, UI_BG);
    ui_card(g, s, x, CARD_Y, w, PROG_H);
    gfx_style hs = { FONT_MEDIUM, 44, UI_BLACK };
    gfx_text_fit(g, s, x + 40, CARD_Y + 88, w - 80, hs, v->title);
    prog_pos p;
    prog_layout(v, &p);
    for (int i = 0; i < PR_COUNT; i++)
        prog_row(r, s, v, &p, i);
    r->shown = *v;
}

static int view_shape_differs(const progress_view *a, const progress_view *b)
{
    return strcmp(a->title, b->title) || !a->file[0] != !b->file[0] || !a->counts[0] != !b->counts[0] ||
           (a->frac >= 0) != (b->frac >= 0);
}

/* Ход на экране: форма карточки изменилась — перерисовать её целиком, иначе (не чаще раза в
 * PAINT_MS или при сдвиге на ≥ 5 %) — только изменившиеся строки одним обновлением. */
static void prog_paint(sync_page *r, pf_screen *s, const progress_view *v, double now)
{
    int w = s->width - 2 * UI_MARGIN;
    if (view_shape_differs(v, &r->shown)) {
        draw_progress_card(r, s, v);
        r->last_paint = now;
        pf_update(UI_MARGIN - 8, CARD_Y - 4, w + 16, PROG_H + 8, PF_UPDATE_PARTIAL);
        return;
    }
    int tw = w - 80;
    int bar_old = (int)(r->shown.frac * (float)tw), bar_new = (int)(v->frac * (float)tw);
    int d[PR_COUNT] = { strcmp(v->file, r->shown.file) != 0, strcmp(v->counts, r->shown.counts) != 0,
                        v->frac >= 0 && bar_old != bar_new, strcmp(v->total, r->shown.total) != 0 };
    int any = d[0] | d[1] | d[2] | d[3];
    if (!any)
        return;
    if (now - r->last_paint < PAINT_MS && fabsf(v->frac - r->shown.frac) < 0.05f)
        return;
    prog_pos p;
    prog_layout(v, &p);
    int y0 = 1 << 30, y1 = 0, x = 0, rw = 0;
    for (int i = 0; i < PR_COUNT; i++) {
        if (!d[i])
            continue;
        ui_rect a = prog_row(r, s, v, &p, i);
        if (a.y < y0)
            y0 = a.y;
        if (a.y + a.h > y1)
            y1 = a.y + a.h;
        x = a.x, rw = a.w;
    }
    r->shown = *v;
    r->last_paint = now;
    pf_update(x, y0, rw, y1 - y0, PF_UPDATE_PARTIAL);
}

/* Значок результата в круге: успех — чёрный круг с «галочкой», сбой — кольцо с «!». */
static void result_icon(gfx *g, pf_screen *s, int x, int y, int size, int ok)
{
    static const float vb[4] = { 0, 0, 24, 24 };
    if (ok) {
        gfx_rrect(g, s, x, y, size, size, size / 2.0f, UI_BLACK);
        gfx_svg(g, s, "M6.5 12.5 L10.5 16.5 L17.5 8", vb, x, y, size, 2.6f, UI_WHITE);
    } else {
        gfx_rrect_line(g, s, x, y, size, size, size / 2.0f, 4, UI_BLACK);
        gfx_svg(g, s, "M12 6.5 L12 13.5 M12 17 L12 17.5", vb, x, y, size, 2.8f, UI_BLACK);
    }
}

/* Подписанный блок итога внутри карточки; возвращает y после него. draw == 0 — только высота. */
static int draw_block(sync_page *r, pf_screen *s, int draw, int tx, int tw, int y, int limit, const char *cap,
                      const block *b, int with_time)
{
    gfx *g = r->env.g;
    pf_screen *sc = draw ? s : NULL;
    gfx_style capst = { FONT_MEDIUM, 32, UI_TEXT2 }, hs = { FONT_MEDIUM, 38, UI_BLACK },
              ts = { FONT_REGULAR, 36, UI_BLACK }, ws = { FONT_REGULAR, 32, UI_TEXT2 };
    if (draw) {
        gfx_text(g, s, tx, y, capst, cap);
        if (with_time && b->time) {
            char when[48];
            format_time(b->time, when, sizeof when);
            gfx_text(g, s, tx + tw - gfx_text_width(g, ws, when), y, ws, when);
        }
    }
    y += 70;
    for (int i = 0; i < b->n && y < limit; i++) {
        int lines = gfx_text_wrap(g, sc, tx, y, tw, 48, 2, i == 0 ? hs : ts, b->line[i]);
        y += 48 * lines + 12;
    }
    return y;
}

/* Карточка итога: последняя удачная синхронизация и (отдельно) неудачная попытка. Возвращает
 * высоту; draw == 0 — только посчитать. */
static int result_card(sync_page *r, pf_screen *s, int draw)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    int tx = x + 40, tw = w - 80;
    int y0 = CARD_Y, y = y0 + 64, limit = y0 + CARD_MAX - 90;
    gfx_style hs = { FONT_MEDIUM, 38, UI_BLACK }, ts = { FONT_REGULAR, 36, UI_BLACK };
    block fail_now;
    const block *att = r->att.time ? &r->att : NULL;
    const char *att_cap = r->att.cancelled ? "Остановлено" : "Последняя попытка не удалась";
    if (r->msg[0]) { /* не удалось даже начать */
        memset(&fail_now, 0, sizeof fail_now);
        fail_now.n = 1;
        snprintf(fail_now.line[0], sizeof fail_now.line[0], "%.255s", r->msg);
        att = &fail_now;
        att_cap = "Не получилось начать";
    }
    if (!r->last.time && !att) {
        if (draw) {
            gfx_style cap = { FONT_MEDIUM, 32, UI_TEXT2 };
            gfx_text(g, s, tx, y, cap, "Синхронизация");
            gfx_text(g, s, tx, y + 80, hs, "Ещё не синхронизировали");
        }
        int n = gfx_text_wrap(g, draw ? s : NULL, tx, y + 144, tw, 48, 4, ts,
                              "Нажмите «Синхронизировать»: книги с сервера появятся на устройстве, а места "
                              "чтения и закладки сведутся.");
        return y + 144 + 48 * n - y0 + 24;
    }
    if (r->last.time) {
        y = draw_block(r, s, draw, tx, tw, y, limit, "Последняя удачная синхронизация", &r->last, 1);
        if (att)
            y += 12;
    }
    if (att) {
        if (r->last.time) {
            if (draw)
                gfx_fill(s, tx, y - 30, tw, 2, UI_FAINT);
            y += 30;
        }
        y = draw_block(r, s, draw, tx, tw, y, limit + 200, att_cap, att, att != &fail_now);
    }
    int h = y - y0 - 24 + 40;
    return h < 260 ? 260 : h > CARD_MAX ? CARD_MAX : h;
}

static void draw_button_pair(sync_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    int y = CARD_Y + r->card_h + 32;
    memset(&r->r_edit, 0, sizeof r->r_edit);
    memset(&r->r_sync, 0, sizeof r->r_sync);
    if (r->cancelling) {
        ui_pill(g, s, x, y, w, 120, "Останавливаем…", 0); /* нажать нечего */
        return;
    }
    r->r_sync = (ui_rect){ x, y, w, 120 };
    const char *label = r->job || r->net_wait ? "Отмена" : r->net_choose ? "Выбрать сеть Wi-Fi" : "Синхронизировать";
    ui_pill(g, s, x, y, w, 120, label, !r->job && !r->net_wait);
    if (!r->job) {
        r->r_edit = (ui_rect){ x, y + 148, w, 110 };
        ui_pill(g, s, x, y + 148, w, 110, "Изменить сервер", 0);
    }
}

/* Строка настройки: подпись и значение (длинное — с «…» посередине, конец виден). */
static void draw_row(sync_page *r, pf_screen *s, int x, int y, int w, const char *title, const char *sub,
                     int unset, int chevron, int line)
{
    gfx *g = r->env.g;
    int pad = 40, right = x + w - pad;
    if (chevron) {
        gfx_icon(g, s, ICON_NEXT, right - 40, y + ROW_H / 2 - 20, 40, 0, UI_TEXT3);
        right -= 56;
    }
    gfx_style ts = { FONT_REGULAR, 40, UI_BLACK }, ss = { FONT_REGULAR, 32, unset ? UI_TEXT3 : UI_TEXT2 };
    gfx_text_fit(g, s, x + pad, y + ROW_H / 2 - 6, right - x - pad, ts, title);
    gfx_text_fit_middle(g, s, x + pad, y + ROW_H / 2 + 38, right - x - pad, ss, sub);
    if (line)
        gfx_fill(s, x + pad, y + ROW_H - 2, w - pad, 2, UI_FAINT);
}

static void draw_main(sync_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    r->hits = ui_header(g, s, "Синхронизация", 1, 0, 1);
    char shown[1100], dir[1100];
    remote_display(r->url, shown, sizeof shown);
    local_dir(r, dir, sizeof dir);
    ui_card(g, s, x, 230, w, 2 * ROW_H);
    draw_row(r, s, x, 230, w, "Сервер", shown, 0, 0, 1);
    draw_row(r, s, x, 230 + ROW_H, w, "Папка на устройстве", dir, 0, 0, 0);
    if (r->job) {
        sync_status st;
        sync_job_status(r->job, &st);
        progress_view v;
        make_view(r, &st, &v);
        r->card_h = PROG_H;
        draw_progress_card(r, s, &v);
        r->last_paint = clock_ms();
    } else {
        r->card_h = result_card(r, s, 0);
        ui_card(g, s, x, CARD_Y, w, r->card_h);
        result_card(r, s, 1);
    }
    draw_button_pair(r, s);
    gfx_style hs = { FONT_REGULAR, 34, UI_TEXT2 };
    int y = CARD_Y + r->card_h + 32 + (r->job ? 120 : 148 + 110) + 44;
    gfx_text_wrap(g, s, x + 8, y + 30, w - 16, 44, 4, hs,
                  "Книги скачиваются с сервера на устройство. Удалённое на сервере остаётся на устройстве, "
                  "а изменённое на устройстве не перезаписывается. Места чтения и закладки сводятся в обе стороны.");
}

static void password_dots(const char *p, char *out, size_t len)
{
    if (!*p) {
        snprintf(out, len, "Не задан");
        return;
    }
    int chars = 0;
    for (const char *q = p; *q; q++)
        chars += ((unsigned char)*q & 0xc0) != 0x80; /* символы, а не байты UTF-8 */
    size_t k = 0;
    for (int i = 0; i < chars && i < 12 && k + 4 < len; i++) {
        memcpy(out + k, "•", 3);
        k += 3;
    }
    out[k] = 0;
}

static void draw_setup(sync_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    int bottom = ui_content_bottom(s);
    int done_y = bottom - 24 - 120;
    r->hits = ui_header(g, s, "Сервер", 1, 0, 1);
    char shown[1100], dir[1100], pw[64];
    if (r->url[0])
        remote_display(r->url, shown, sizeof shown);
    else
        snprintf(shown, sizeof shown, "Не задан");
    local_dir(r, dir, sizeof dir);
    password_dots(r->password, pw, sizeof pw);
    int y = 230;
    if (!r->url[0]) { /* первый вход: что это и что нужно */
        gfx_style is = { FONT_REGULAR, 36, UI_BLACK };
        int n = gfx_text_wrap(g, s, x + 8, y + 36, w - 16, 50, 4, is,
                              "Книги из папки в облаке или на домашнем сервере будут появляться на "
                              "устройстве. Нужен адрес папки; логин и пароль — если сервер их просит.");
        y += 50 * n + 36;
    }
    ui_card(g, s, x, y, w, 4 * ROW_H);
    const char *titles[F_COUNT] = { "Адрес папки", "Логин", "Пароль", "Папка на устройстве" };
    const char *subs[F_COUNT] = { shown, r->login[0] ? r->login : r->lost & 1 ? "Введите заново" : "Не задан",
                                  r->lost & 2 ? "Введите заново" : pw, dir };
    int unset[F_COUNT] = { !r->url[0], !r->login[0], !r->password[0], 0 };
    for (int i = 0; i < F_COUNT; i++) {
        draw_row(r, s, x, y + i * ROW_H, w, titles[i], subs[i], unset[i], 1, i + 1 < F_COUNT);
        r->rows[i] = (ui_rect){ x, y + i * ROW_H, w, ROW_H };
    }
    y += 4 * ROW_H + 28;
    r->r_check = (ui_rect){ x, y, w, 110 };
    int checking = r->chk.started && !r->chk.done;
    ui_pill(g, s, x, y, w, 110,
            checking ? "Проверяем…" : r->net_wait ? "Отмена" : r->net_choose ? "Выбрать сеть Wi-Fi" : "Проверить", 0);
    y += 110 + 24;
    if (r->chk_shown || checking) {
        const char *msg = checking ? "Подключаемся…" : r->chk.msg;
        int icon = !checking;
        gfx_style rs = { FONT_MEDIUM, 36, UI_BLACK };
        int tx = x + 40 + (icon ? 84 : 0), tw = w - 80 - (icon ? 84 : 0);
        int n = gfx_text_wrap(g, NULL, tx, 0, tw, 48, 4, rs, msg);
        int h = 48 * n + 56 > 120 ? 48 * n + 56 : 120;
        ui_card(g, s, x, y, w, h);
        if (icon)
            result_icon(g, s, x + 40, y + h / 2 - 30, 60, r->chk.ok);
        gfx_text_wrap(g, s, tx, y + h / 2 - 24 * (n - 1) + 12, tw, 48, 4, rs, msg);
        y += h + 24;
    }
    int limit = (r->url[0] ? done_y : bottom) - 16;
    gfx_style hs = { FONT_REGULAR, 34, UI_TEXT2 };
    static const char *const hints[] = {
        "Облачная папка (WebDAV): Яндекс Диск, Nextcloud, свой сервер. Адрес начинается с https://",
        "Яндекс Диск: webdav.yandex.ru/Книги. Вместо пароля — «пароль приложения» из настроек аккаунта.",
        "Nextcloud: хост/remote.php/dav/files/ЛОГИН/Книги",
        "Свой FTP-сервер: ftp://хост/папка",
    };
    for (int i = 0; i < 4; i++) {
        int lines = gfx_text_wrap(g, NULL, x + 8, 0, w - 16, 44, 3, hs, hints[i]);
        if (y + 44 * lines > limit)
            break;
        gfx_text_wrap(g, s, x + 8, y + 32, w - 16, 44, 3, hs, hints[i]);
        y += 44 * lines + 14;
    }
    if (r->url[0]) { /* без адреса выходить есть чем: «‹» в заголовке */
        r->r_done = (ui_rect){ x, done_y, w, 120 };
        ui_pill(g, s, x, done_y, w, 120, "Готово", 1);
    }
}

/* «Остановить синхронизацию?» — карточка по центру на чистом фоне. */
static void draw_confirm(sync_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, h = 500;
    int top = ui_content_top(), bottom = ui_content_bottom(s);
    int y = top + (bottom - top - h) / 2;
    ui_card(g, s, x, y, w, h);
    int tx = x + 48, tw = w - 96;
    gfx_style hs = { FONT_MEDIUM, 46, UI_BLACK }, ts = { FONT_REGULAR, 36, UI_TEXT2 };
    int n = gfx_text_wrap(g, s, tx, y + 90, tw, 58, 2, hs, "Остановить синхронизацию?");
    gfx_text_wrap(g, s, tx, y + 90 + 58 * n + 20, tw, 48, 3, ts,
                  "Уже скачанные книги останутся на устройстве. Остальное можно докачать в другой раз.");
    /* Как во всех вопросах «Остановить?»: в ряд, главная (чёрная) — безопасная «Продолжить». */
    int bw = (tw - 24) / 2, by = y + h - 48 - 110;
    r->r_yes = (ui_rect){ tx, by, bw, 110 };
    r->r_no = (ui_rect){ tx + bw + 24, by, bw, 110 };
    ui_pill(g, s, r->r_yes.x, by, bw, 110, "Остановить", 0);
    ui_pill(g, s, r->r_no.x, by, bw, 110, "Продолжить", 1);
}

static void sync_draw(page *p, pf_screen *s)
{
    sync_page *r = (sync_page *)p;
    gfx_fill(s, 0, UI_STATUS_H, s->width, ui_content_bottom(s) - UI_STATUS_H, UI_BG);
    memset(&r->r_sync, 0, sizeof r->r_sync);
    memset(&r->r_edit, 0, sizeof r->r_edit);
    memset(&r->r_check, 0, sizeof r->r_check);
    memset(&r->r_done, 0, sizeof r->r_done);
    memset(&r->r_yes, 0, sizeof r->r_yes);
    memset(&r->r_no, 0, sizeof r->r_no);
    memset(&r->hits, 0, sizeof r->hits);
    memset(r->rows, 0, sizeof r->rows);
    if (r->confirm && r->job)
        draw_confirm(r, s);
    else if (r->view == V_SETUP)
        draw_setup(r, s);
    else
        draw_main(r, s);
}

/* ---- Ввод ---- */

static void kbd_done(const char *text, void *user)
{
    kbd_ctx *k = user;
    sync_page *r = k->page;
    if (text) {
        char val[1100];
        snprintf(val, sizeof val, "%s", text);
        size_t n = strlen(val);
        while (n && (val[n - 1] == ' ' || val[n - 1] == '\n'))
            val[--n] = 0;
        char *v = val;
        while (*v == ' ')
            v++;
        r->chk_shown = 0;
        switch (k->field) {
        case F_URL: {
            char url[1024], err[160];
            if (remote_normalize(v, url, sizeof url, err, sizeof err) != 0) {
                snprintf(r->chk.msg, sizeof r->chk.msg, "Адрес не сохранён. %s.", err);
                r->chk.ok = 0;
                r->chk_shown = 1;
            } else {
                if (strcmp(url, r->url))
                    forget_state(r);
                snprintf(r->url, sizeof r->url, "%s", url);
                save_all(r);
            }
            break;
        }
        case F_LOGIN:
            if (strcmp(v, r->login) && !(r->lost & 1)) /* не расшифровался — скорее всего, тот же */
                forget_state(r);
            r->lost &= ~1;
            snprintf(r->login, sizeof r->login, "%s", v);
            save_all(r);
            break;
        case F_PASSWORD:
            r->lost &= ~2;
            snprintf(r->password, sizeof r->password, "%s", v);
            save_all(r);
            break;
        default:
            snprintf(r->dir, sizeof r->dir, "%s", v);
            save_all(r);
            break;
        }
    }
    if (r->open)
        r->env.redraw(r->env.user, 1);
}

static void edit_field(sync_page *r, int f)
{
    static const char *const titles[F_COUNT] = { "Адрес папки на сервере", "Логин", "Пароль",
                                                 "Папка на устройстве" };
    char cur[1100];
    switch (f) {
    case F_URL: snprintf(cur, sizeof cur, "%s", r->url); break;
    case F_LOGIN: snprintf(cur, sizeof cur, "%s", r->login); break;
    case F_PASSWORD: snprintf(cur, sizeof cur, "%s", r->password); break;
    default: local_dir(r, cur, sizeof cur); break;
    }
    r->kbd[f] = (kbd_ctx){ r, f };
    pf_keyboard(titles[f], cur, f == F_URL || f == F_DIR ? 1000 : 120, kbd_done, &r->kbd[f]);
}

enum { NET_FOR_CHECK = 1, NET_FOR_SYNC };

static void net_msg(sync_page *r, int what, const char *text)
{
    if (what == NET_FOR_CHECK) {
        snprintf(r->chk.msg, sizeof r->chk.msg, "%s", text);
        r->chk.ok = 0;
        r->chk_shown = 1;
    } else {
        snprintf(r->msg, sizeof r->msg, "%s", text);
    }
}

/* Сеть для проверки или синхронизации, не блокируя экран. 1 — есть, продолжать сейчас; 0 — нет
 * (подключаемся в фоне — продолжит tick; или не вышло — сообщение и «Выбрать сеть Wi-Fi»). */
static int net_ready(sync_page *r, int what)
{
    int rc;
    if (r->net_choose) { /* окно прошивки: блокирует, пока человек выбирает сеть */
        r->net_choose = 0;
        rc = pf_net_connect() == 0 ? 1 : -1;
        r->env.redraw(r->env.user, 1);
    } else {
        rc = pf_net_start();
    }
    if (rc == 1)
        return 1;
    if (rc == 0) {
        r->net_wait = 1;
        r->net_for = what;
        net_msg(r, what, "Подключаюсь к Wi-Fi…");
    } else {
        r->net_choose = 1;
        net_msg(r, what, "Известной сети Wi-Fi рядом нет — выберите сеть.");
    }
    r->env.redraw(r->env.user, 0);
    return 0;
}

static void start_check(sync_page *r)
{
    if (!r->url[0]) {
        snprintf(r->chk.msg, sizeof r->chk.msg, "Сначала введите адрес папки.");
        r->chk.ok = 0;
        r->chk_shown = 1;
        r->env.redraw(r->env.user, 0);
        return;
    }
    if (lost_hint(r, r->chk.msg, sizeof r->chk.msg)) {
        r->chk.ok = 0;
        r->chk_shown = 1;
        r->env.redraw(r->env.user, 0);
        return;
    }
    check_join(r);
    if (!net_ready(r, NET_FOR_CHECK))
        return;
    memset(&r->chk, 0, sizeof r->chk);
    remote_of(r, &r->chk.cfg);
    if (pthread_create(&r->chk.th, NULL, check_run, &r->chk) == 0) {
        r->chk.started = 1;
        r->chk_shown = 1;
    }
    r->env.redraw(r->env.user, 0);
}

/* 1 — экран надо перерисовать (не вышло начать), 0 — уже перерисован или не нужно. */
static int start_sync(sync_page *r)
{
    r->msg[0] = 0;
    if (lost_hint(r, r->msg, sizeof r->msg))
        return 1;
    char why[160];
    if (remote_is_ftp(r->url) && !net_has_protocol("ftp")) {
        snprintf(r->msg, sizeof r->msg, "В прошивке нет FTP — укажите адрес облачной папки (WebDAV): https://…");
        return 1;
    }
    if (net_http_ready(why, sizeof why) != 0) {
        snprintf(r->msg, sizeof r->msg, "%s", why);
        return 1;
    }
    if (!net_ready(r, NET_FOR_SYNC))
        return 0;
    sync_job_cfg c;
    memset(&c, 0, sizeof c);
    remote_of(r, &c.remote);
    local_dir(r, c.local_dir, sizeof c.local_dir);
    snprintf(c.data_dir, sizeof c.data_dir, "%s", r->env.data_dir);
    r->job = sync_job_start(&c);
    if (!r->job) {
        snprintf(r->msg, sizeof r->msg, "Не хватило памяти.");
        return 1;
    }
    r->merge_done = r->merge_failed = r->cancelling = 0;
    memset(&r->nres, 0, sizeof r->nres);
    r->last_paint = clock_ms();
    log_msg("синхронизация: начало, %s → %s", r->url, c.local_dir);
    pf_keep_awake(60);
    r->last_awake = clock_ms();
    r->env.redraw(r->env.user, 0);
    return 0;
}

/* ---- Страница ---- */

static void sync_open(page *p)
{
    sync_page *r = (sync_page *)p;
    r->open = 1;
    r->confirm = 0;
    r->msg[0] = 0;
    r->net_wait = r->net_choose = 0;
    r->view = r->url[0] ? V_MAIN : V_SETUP;
}

static void sync_close(page *p)
{
    sync_page *r = (sync_page *)p;
    r->open = 0;
    r->confirm = 0;
    if (r->job) {
        sync_job_cancel(r->job); /* поток остановится сам; итог заберёт tick, когда страницу откроют снова */
        r->cancelling = 1;
    }
}

/* Уйти хочет хаб (вкладка, «Домой»): идёт синхронизация — спросить. */
static int sync_leave(page *p)
{
    sync_page *r = (sync_page *)p;
    if (!r->job || r->cancelling)
        return 0;
    r->confirm = 1;
    return 1;
}

static void flash(sync_page *r, ui_rect a)
{
    pf_screen *s = pf_get_screen();
    if (s)
        ui_flash(r->env.g, s, a);
}

static int sync_tap(page *p, int x, int y)
{
    sync_page *r = (sync_page *)p;
    if (r->confirm) {
        if (!r->job) { /* пока спрашивали, синхронизация кончилась */
            r->confirm = 0;
            return PAGE_REDRAW;
        }
        if (ui_inside(r->r_yes, x, y)) {
            flash(r, r->r_yes);
            sync_job_cancel(r->job);
            r->cancelling = 1;
            r->confirm = 0;
            return PAGE_CLOSE;
        }
        if (ui_inside(r->r_no, x, y)) {
            flash(r, r->r_no);
            r->confirm = 0;
            return PAGE_REDRAW;
        }
        return 0;
    }
    if (ui_inside(r->hits.back, x, y)) {
        if (r->view == V_SETUP && r->url[0]) {
            r->view = V_MAIN;
            return PAGE_REDRAW;
        }
        if (r->job && !r->cancelling) {
            r->confirm = 1;
            return PAGE_REDRAW;
        }
        return PAGE_CLOSE;
    }
    if (r->view == V_SETUP) {
        int checking = r->chk.started && !r->chk.done; /* пока идёт проверка, настройки не меняем */
        for (int i = 0; i < F_COUNT; i++)
            if (!checking && ui_inside(r->rows[i], x, y)) {
                flash(r, r->rows[i]);
                edit_field(r, i);
                return 0;
            }
        if (ui_inside(r->r_check, x, y) && !checking) {
            flash(r, r->r_check);
            if (r->net_wait) { /* «Отмена» ожидания сети */
                r->net_wait = 0;
                r->chk_shown = 0;
                return PAGE_REDRAW;
            }
            start_check(r);
            return 0;
        }
        if (ui_inside(r->r_done, x, y)) {
            flash(r, r->r_done);
            r->view = V_MAIN;
            return PAGE_REDRAW;
        }
        return 0;
    }
    if (ui_inside(r->r_sync, x, y)) {
        flash(r, r->r_sync);
        if (r->job) {
            sync_job_cancel(r->job);
            r->cancelling = 1;
            return PAGE_REDRAW;
        }
        if (r->net_wait) { /* «Отмена» ожидания сети */
            r->net_wait = 0;
            r->msg[0] = 0;
            return PAGE_REDRAW;
        }
        return start_sync(r) ? PAGE_REDRAW : 0;
    }
    if (!r->job && ui_inside(r->r_edit, x, y)) {
        flash(r, r->r_edit);
        r->view = V_SETUP;
        r->chk_shown = 0;
        return PAGE_REDRAW;
    }
    return 0;
}

static int sync_key(page *p, pf_key key)
{
    (void)p;
    (void)key;
    return 0;
}

/* Слияние мест чтения — в потоке интерфейса, пока поток задачи ждёт. */
static void do_merge(sync_page *r)
{
    r->merge_done = 1;
    if (!r->env.pos->items) {
        sync_job_merged(r->job, -1, NULL);
        r->merge_failed = 1;
        return;
    }
    int rc = notes_merge(r->env.pos, sync_job_files(r->job), &r->nres);
    log_msg("синхронизация: слияние → %d (мест обновлено %d, добавлено %d, закладок %d; отправить: %d %d)", rc,
            r->nres.pos_updated, r->nres.pos_imported, r->nres.bm_updated, r->nres.send_pos, r->nres.send_bm);
    r->merge_failed = rc != 0;
    sync_job_merged(r->job, rc, &r->nres);
}

static int sync_tick(page *p, pf_screen *s)
{
    sync_page *r = (sync_page *)p;
    if (r->net_wait) {
        int rc = pf_net_poll();
        if (rc == 0)
            return 0;
        r->net_wait = 0;
        if (rc < 0) {
            r->net_choose = 1;
            net_msg(r, r->net_for, "Известной сети Wi-Fi рядом нет — выберите сеть.");
            return PAGE_REDRAW;
        }
        r->msg[0] = 0;
        if (r->net_for == NET_FOR_CHECK)
            start_check(r);
        else if (start_sync(r))
            return PAGE_REDRAW;
        return 0;
    }
    if (r->chk.started && r->chk.done) {
        check_join(r);
        return PAGE_REDRAW;
    }
    if (!r->job)
        return 0;
    double now = clock_ms();
    if (now - r->last_awake > 5000) {
        pf_keep_awake(60);
        r->last_awake = now;
    }
    sync_status st;
    sync_job_status(r->job, &st);
    if (st.stage == SJ_MERGE && !r->merge_done)
        do_merge(r);
    if (st.stage == SJ_DONE) {
        sync_job_free(r->job);
        r->job = NULL;
        r->cancelling = 0;
        log_msg("синхронизация: конец (новых %d, обновлено %d, не скачано %d, ошибка «%s»)", st.n_new,
                st.n_updated, st.n_failed, st.error);
        summarize(r, &st);
        int leaving = r->confirm; /* человек уходил, а она кончилась сама — пусть уходит */
        r->confirm = 0;
        return (leaving ? PAGE_CLOSE : PAGE_REDRAW) | (st.n_new + st.n_updated ? PAGE_BOOKS : 0);
    }
    if (r->view != V_MAIN || r->confirm)
        return 0;
    progress_view v;
    make_view(r, &st, &v);
    prog_paint(r, s, &v, now);
    return 0;
}

static const char *sync_selected(page *p)
{
    (void)p;
    return NULL;
}

static void sync_free(page *p)
{
    sync_page *r = (sync_page *)p;
    sync_job_free(r->job);
    check_join(r);
    free(r);
}

static const page_ops ops = { sync_open, sync_close, sync_draw, sync_tap, sync_key, sync_tick, sync_selected,
                              sync_free, sync_leave };

page *sync_page_new(const page_env *env)
{
    sync_page *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->base.ops = &ops;
    r->base.gray = 1;
    r->env = *env;
    snprintf(r->cfg_path, sizeof r->cfg_path, "%s/sync.txt", env->data_dir);
    load_settings(r);
    return &r->base;
}
