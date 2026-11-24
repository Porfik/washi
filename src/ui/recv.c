/* Приём по Wi-Fi: на устройстве поднимаются два сервера — страница в браузере (httpd.c) и FTP
 * (ftpd.c). Экран показывает адрес, QR-код, что уже принято и ход текущей передачи.
 *
 * Серверы работают в своих потоках и пишут в общий журнал (xfer.h); tick раз в 500 мс
 * забирает снимок журнала. Ход передачи — частичное обновление только строки передачи, не
 * чаще раза в 2 с; начало и конец передачи — обновление карточки «Принятые книги» целиком.
 * Уходя со страницы, серверы останавливаются (идёт передача — сначала вопрос «Остановить?»).
 * Нет передач 15 минут — тоже останавливаются; Wi-Fi, который включил сам приём, выключается.
 *
 * Папка приёма: общая папка загрузок хаба или своя (dirsel, выбор — <папка данных>/recv_dir.txt);
 * смена перезапускает серверы (когда нет передач).
 *
 * Пароль (по желанию): <папка данных>/recv.txt, зашифрован (secret.h). Задан — страница и FTP
 * спрашивают его (логин любой); смена действует сразу — пароль лежит в журнале приёма (xfer.h).
 *
 * Переключатель «Разрешить удалять и переименовывать из браузера» (там же, recv.txt: edit=1; по
 * умолчанию выключен): без него страница в браузере только смотрит папки, создаёт их и загружает.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../base/clock.h"
#include "../base/log.h"
#include "../net/ftpd.h"
#include "../net/httpd.h"
#include "../net/net.h"
#include "../net/xfer.h"
#include "../vendor/qrcodegen.h"
#include "folderpick.h"
#include "page.h"
#include "widgets.h"

#ifndef IDLE_STOP_MS /* (переопределяется при сборке для проверки) */
#define IDLE_STOP_MS (15 * 60 * 1000.0) /* без передач столько — остановить серверы */
#endif
#define PAINT_MIN_MS 1000.0             /* начало и конец передач на экране — не чаще */
#define ROW_PAINT_MS 2000.0             /* ход передачи (строка) на экране — не чаще */
#define AWAKE_EVERY_MS 10000.0
#define AWAKE_SEC 30
#define IP_CHECK_MS 5000.0
#define QR_MAX_VERSION 8
#define QR_BUDGET 600 /* сторона QR-кода вместе с полем, точек */

typedef enum {
    ST_CONNECT, /* подключаемся к Wi-Fi и запускаем серверы (в ближайшем tick) */
    ST_RUN,
    ST_NOWIFI,
    ST_IDLE, /* остановлено из-за простоя */
    ST_FAIL, /* серверы не запустились */
} state;

typedef struct {
    page base;
    page_env env;
    ui_header_hits hits;
    state st;
    xfer log;
    httpd *http;
    ftpd *ftp;
    char ip[64];
    int http_port, ftp_port;
    char url[128], ftp_url[128];
    uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    int qr_ok;
    ui_rect button;   /* «Закончить приём», «Подключиться», «Начать снова» */
    ui_rect card;     /* карточка «Принятые книги» — её обновляем при передаче */
    int books_gen;    /* на какой версии библиотеке сообщали о новых книгах */
    double painted_ms, awake_ms, ip_ms;
    char ftp_dir[128]; /* папка, куда попадают файлы по FTP (последняя часть пути); пусто — корень */
    int connecting;    /* ST_CONNECT: подключение к Wi-Fi уже идёт (pf_net_poll) */
    time_t tried_at;   /* когда не вышло подключиться (0 — не пробовали) */
    int confirm;       /* на экране вопрос «Остановить приём?» */
    ui_rect ov_stop, ov_go;
    int open;
    char pass[256];    /* пароль приёма (открытый — только в памяти); пусто — без пароля */
    char pass_file[1200];
    ui_rect pw_btn;    /* «Пароль: нет / задан» */
    ui_rect ed_btn;    /* строка-переключатель «удалять и переименовывать из браузера» */
    int edit;          /* переключатель включён */
    ui_rect dir_btn;   /* «Папка: Downloads (общая)» */
    dirsel *ds;        /* общая или своя папка приёма */
    int pass_lost;     /* пароль в файле не расшифровался: приём закрыт случайным паролем */
    int menu;          /* на экране выбор «Задать / Убрать пароль» */
    ui_rect m_set, m_clear, m_cancel;
    /* Что нарисовано в карточке: структура (сколько идёт и принято) и по строке на передачу. */
    int sh_nactive, sh_total_done, sh_valid;
    int sh_id[XFER_ACTIVE_MAX], sh_pct[XFER_ACTIVE_MAX];
    long long sh_size[XFER_ACTIVE_MAX];
    double sh_ms[XFER_ACTIVE_MAX];
    ui_rect arow[XFER_ACTIVE_MAX];
} recv_page;

/* ---- серверы ---- */

static void stop_servers(recv_page *r)
{
    if (r->http)
        httpd_stop(r->http);
    if (r->ftp)
        ftpd_stop(r->ftp);
    r->http = NULL;
    r->ftp = NULL;
}

static void make_urls(recv_page *r)
{
    snprintf(r->url, sizeof r->url, "http://%s:%d", r->ip, r->http_port);
    snprintf(r->ftp_url, sizeof r->ftp_url, "ftp://%s:%d", r->ip, r->ftp_port);
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION)];
    r->qr_ok = qrcodegen_encodeText(r->url, tmp, r->qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                                    QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
}

/* Сеть есть (rc == 0) или нет — запустить серверы; r->st — что получилось. */
static void launch(recv_page *r, int rc)
{
    r->connecting = 0;
    r->st = ST_NOWIFI;
    r->confirm = 0;
    if (rc != 0 || net_local_ip(r->ip, sizeof r->ip) != 0) {
        log_msg("приём: нет Wi-Fi");
        r->tried_at = time(NULL);
    } else {
        r->tried_at = 0;
        xfer_destroy(&r->log);
        xfer_init(&r->log);
        xfer_set_password(&r->log, r->pass);
        xfer_set_allow_edit(&r->log, r->edit);
        r->books_gen = 0;
        r->sh_valid = 0;
        char dir[1024];
        dirsel_dir(r->ds, dir, sizeof dir);
        net_mkdirs(dir);
        log_msg("приём: папка %s", dir);
        r->http = httpd_start(&r->log, dir, r->env.library_root, r->env.data_dir, HTTPD_PORT);
        if (!r->http) {
            r->st = ST_FAIL;
        } else {
            /* FTP начинается в папке приёма (если она внутри библиотеки), выше — вся библиотека. */
            r->ftp = ftpd_start(&r->log, r->env.library_root, r->env.data_dir, dir, FTPD_PORT);
            r->ftp_dir[0] = 0;
            if (r->ftp) {
                const char *home = ftpd_home(r->ftp), *slash = strrchr(home, '/');
                snprintf(r->ftp_dir, sizeof r->ftp_dir, "%s", slash ? slash + 1 : home);
            }
            r->http_port = httpd_port(r->http);
            r->ftp_port = r->ftp ? ftpd_port(r->ftp) : 0;
            make_urls(r);
            r->st = ST_RUN;
            r->awake_ms = 0;
            r->ip_ms = clock_ms();
            xfer_touch(&r->log);
            log_msg("приём: %s%s%s", r->url, r->ftp ? ", " : "", r->ftp ? r->ftp_url : "");
        }
    }
}

/* Начать подключение к Wi-Fi, не блокируя экран: сеть есть — сразу серверы, иначе ждём в tick
 * (на экране «Подключаюсь…» и «Отмена»). 1 — состояние сменилось (перерисовать). */
static int begin(recv_page *r)
{
    stop_servers(r);
    int rc = pf_net_start();
    if (rc == 0) {
        r->connecting = 1;
        return 0;
    }
    launch(r, rc == 1 ? 0 : -1);
    return 1;
}

/* «Выбрать сеть»: окно прошивки (блокирует, пока человек выбирает), потом — с миганием. */
static void choose_network(recv_page *r)
{
    int rc = pf_net_connect();
    launch(r, rc);
    r->env.redraw(r->env.user, 1);
}

/* ---- рисование ---- */

#define CARD_GAP 24
#define DONE_H UI_ROW_H /* строка принятого */
#define MORE_H 80       /* строка «и ещё N» */
#define BTN_H 100
#define ED_H 112 /* строка-переключатель */

static int card_x(void)
{
    return UI_MARGIN;
}

/* Текст по центру; длинный — с «…» посередине (адрес: конец виден). */
static void draw_centered(recv_page *r, pf_screen *s, int cy, int size, gfx_font font, uint8_t gray,
                          const char *text)
{
    gfx_style st = { font, (float)size, gray };
    int w = gfx_text_width(r->env.g, st, text);
    gfx_text_fit_middle(r->env.g, s, (s->width - w) / 2 > UI_MARGIN ? (s->width - w) / 2 : UI_MARGIN, cy,
                        s->width - 2 * UI_MARGIN - 48, st, text);
}

/* Крупная карточка с сообщением, значком и (если есть) кнопкой. */
static void draw_message(recv_page *r, pf_screen *s, gfx_icon_id icon, const char *title, const char *text,
                         const char *button)
{
    gfx *g = r->env.g;
    int x = card_x(), w = s->width - 2 * UI_MARGIN, y = ui_content_top() + 16;
    int h = button ? 680 : 520;
    ui_card(g, s, x, y, w, h);
    gfx_icon(g, s, icon, s->width / 2 - 60, y + 56, 120, 0, UI_BLACK);
    draw_centered(r, s, y + 250, 50, FONT_MEDIUM, UI_BLACK, title);
    gfx_style ts = { FONT_REGULAR, 36, UI_TEXT2 };
    gfx_text_wrap_center(g, s, x + 60, y + 322, w - 120, 50, 5, ts, text);
    r->button = (ui_rect){ 0, 0, 0, 0 };
    if (button) {
        int bw = 520, bh = 110;
        r->button = (ui_rect){ (s->width - bw) / 2, y + h - bh - 56, bw, bh };
        ui_pill(g, s, r->button.x, r->button.y, bw, bh, button, 1);
    }
}

/* Строка принятого: название (длинное — «…» посередине), под ним размер и откуда. */
static void draw_done_row(recv_page *r, pf_screen *s, int x, int y, int w, const xfer_item *it, int line)
{
    gfx *g = r->env.g;
    char sz[32], sub[96];
    ui_format_size(sz, sizeof sz, it->size);
    snprintf(sub, sizeof sub, "%s · %s", sz, it->via ? "по FTP" : "из браузера");
    gfx_style ns = { FONT_REGULAR, 38, UI_BLACK }, ss = { FONT_REGULAR, 32, UI_TEXT2 };
    int pad = 40;
    gfx_text_fit_middle(g, s, x + pad, y + 62, w - 2 * pad, ns, it->name);
    gfx_text(g, s, x + pad, y + 110, ss, sub);
    if (line)
        gfx_fill(s, x + pad, y + DONE_H - 2, w - 2 * pad, 2, UI_FAINT);
}

/* Высота строки идущей передачи: с полосой — когда известен размер, иначе без «дырки». */
static int active_h(const xfer_item *it)
{
    return it->total > 0 ? 140 : 112;
}

static int percent_of(const xfer_item *it)
{
    if (it->total <= 0)
        return -1;
    long long p = it->size * 100 / it->total;
    return p > 100 ? 100 : (int)p;
}

/* Идущая передача: название, полоса и «34 % · 12 МБ из 40 МБ». */
static void draw_active_row(recv_page *r, pf_screen *s, int x, int y, int w, const xfer_item *it)
{
    gfx *g = r->env.g;
    int pad = 40;
    char a[32], b[32], st[96];
    ui_format_size(a, sizeof a, it->size);
    gfx_style ns = { FONT_MEDIUM, 38, UI_BLACK }, ss = { FONT_REGULAR, 32, UI_TEXT2 };
    gfx_text_fit_middle(g, s, x + pad, y + 48, w - 2 * pad, ns, it->name);
    if (it->total > 0) {
        ui_format_size(b, sizeof b, it->total);
        snprintf(st, sizeof st, "%d %% · %s из %s", percent_of(it), a, b);
        ui_progress(g, s, x + pad, y + 70, w - 2 * pad, 14, (float)it->size / (float)it->total);
        gfx_text(g, s, x + pad, y + 124, ss, st);
    } else {
        snprintf(st, sizeof st, "принято %s", a);
        gfx_text(g, s, x + pad, y + 96, ss, st);
    }
}

/* Что в карточке изменилось с прошлой отрисовки: структура (началась или кончилась передача). */
static int same_struct(const recv_page *r, const xfer_snap *sn)
{
    if (!r->sh_valid || r->sh_nactive != sn->nactive || r->sh_total_done != sn->total_done)
        return 0;
    for (int i = 0; i < sn->nactive; i++)
        if (r->sh_id[i] != sn->active[i].id)
            return 0;
    return 1;
}

/* Карточка «Принятые книги»: заголовок, «Закончить приём», идущие передачи и последние принятые. */
static void draw_received(recv_page *r, pf_screen *s, const xfer_snap *sn)
{
    gfx *g = r->env.g;
    ui_rect c = r->card;
    /* Сначала фон под карточкой: поверх прежней карточки скруглённые края копили бы белое. */
    gfx_fill(s, c.x, c.y, c.w, c.h, UI_BG);
    ui_card(g, s, c.x, c.y, c.w, c.h);
    char head[64];
    if (sn->total_done > 0)
        snprintf(head, sizeof head, "Принятые книги: %d", sn->total_done);
    else
        snprintf(head, sizeof head, "Принятые книги");
    gfx_style hs = { FONT_MEDIUM, 42, UI_BLACK };
    int bw = 380, bh = BTN_H;
    r->button = (ui_rect){ c.x + c.w - 40 - bw, c.y + 24, bw, bh };
    gfx_text_fit(g, s, c.x + 40, c.y + 24 + bh / 2 + 15, c.w - 80 - bw - 24, hs, head);
    ui_pill(g, s, r->button.x, r->button.y, bw, bh, "Закончить приём", 1);

    r->sh_valid = 1;
    r->sh_nactive = sn->nactive;
    r->sh_total_done = sn->total_done;
    double now = clock_ms();
    int y = c.y + 24 + bh + 24, bottom = c.y + c.h - UI_CARD_R;
    gfx_fill(s, c.x + 40, y - 6, c.w - 80, 2, UI_FAINT);
    for (int i = 0; i < sn->nactive; i++) {
        int h = active_h(&sn->active[i]);
        r->sh_id[i] = sn->active[i].id;
        r->sh_pct[i] = percent_of(&sn->active[i]);
        r->sh_size[i] = sn->active[i].size;
        r->sh_ms[i] = now;
        r->arow[i] = (ui_rect){ 0, 0, 0, 0 };
        if (y + h > bottom)
            continue; /* не влезла: показывается только то, что поместилось */
        r->arow[i] = (ui_rect){ c.x, y, c.w, h };
        draw_active_row(r, s, c.x, y, c.w, &sn->active[i]);
        y += h;
    }
    if (!sn->ndone && !sn->nactive) {
        gfx_style es = { FONT_REGULAR, 36, UI_TEXT2 };
        gfx_text_wrap(g, s, c.x + 40, y + 60, c.w - 80, 52, 3, es,
                      "Пока ничего. Откройте адрес выше и отправьте книги — они появятся здесь.");
        return;
    }
    /* Принятые: последние, сколько влезает; остальные — строкой «и ещё N». */
    int room = bottom - y, k = sn->ndone;
    if (sn->total_done > sn->ndone || k * DONE_H > room) {
        k = (room - MORE_H) / DONE_H;
        if (k < 0)
            k = 0;
        if (k > sn->ndone)
            k = sn->ndone;
    }
    int more = sn->total_done - k;
    for (int i = 0; i < k; i++, y += DONE_H)
        draw_done_row(r, s, c.x, y, c.w, &sn->done[i], i + 1 < k || more > 0);
    if (more > 0 && bottom - y >= MORE_H) {
        char t[128];
        snprintf(t, sizeof t, "и ещё %d — в «Библиотеке»", more);
        gfx_style ms = { FONT_REGULAR, 36, UI_TEXT2 };
        gfx_text_fit(g, s, c.x + 40, y + 52, c.w - 80, ms, t);
    }
}

/* Ход идущих передач: перерисовать только строки, у которых изменилось число, и не чаще раза в
 * ROW_PAINT_MS на строку. Структура та же (same_struct). */
static void paint_progress(recv_page *r, pf_screen *s, const xfer_snap *sn, double now)
{
    for (int i = 0; i < sn->nactive; i++) {
        const xfer_item *it = &sn->active[i];
        ui_rect rr = r->arow[i];
        if (rr.w <= 0 || now - r->sh_ms[i] < ROW_PAINT_MS)
            continue;
        int pct = percent_of(it);
        int changed = pct >= 0 ? pct != r->sh_pct[i] : it->size - r->sh_size[i] >= 256 * 1024;
        if (!changed)
            continue;
        gfx_fill(s, rr.x + 4, rr.y, rr.w - 8, rr.h, UI_WHITE);
        draw_active_row(r, s, rr.x, rr.y, rr.w, it);
        pf_update(rr.x + 4, rr.y, rr.w - 8, rr.h, PF_UPDATE_PARTIAL);
        r->sh_pct[i] = pct;
        r->sh_size[i] = it->size;
        r->sh_ms[i] = now;
    }
}

static void draw_qr(recv_page *r, pf_screen *s, int cx, int top)
{
    if (!r->qr_ok)
        return;
    int n = qrcodegen_getSize(r->qr), m = QR_BUDGET / (n + 8);
    if (m < 1)
        m = 1;
    int x0 = cx - n * m / 2;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(r->qr, x, y))
                gfx_fill(s, x0 + x * m, top + y * m, m, m, UI_BLACK);
}

static int qr_height(const recv_page *r)
{
    if (!r->qr_ok)
        return 0;
    int n = qrcodegen_getSize(r->qr), m = QR_BUDGET / (n + 8);
    return n * (m < 1 ? 1 : m);
}

/* Строка: переключатель «Разрешить удалять и переименовывать из браузера» и пояснение. */
static void draw_edit_row(recv_page *r, pf_screen *s, int x, int y, int w)
{
    gfx *g = r->env.g;
    int sw = 108, sh = 60, d = 44, right = x + w - 40, cy = y + ED_H / 2;
    gfx_fill(s, x + 40, y - 14, w - 80, 2, UI_FAINT);
    gfx_style ts = { FONT_MEDIUM, 36, UI_BLACK }, ns = { FONT_REGULAR, 34, UI_TEXT2 }, vs = { FONT_MEDIUM, 32, UI_BLACK };
    int tw = right - sw - 20 - 90 - (x + 40);
    gfx_text_fit(g, s, x + 40, y + 42, tw, ts, "Разрешить удалять и переименовывать из браузера");
    gfx_text_fit(g, s, x + 40, y + 92, tw, ns, "Лучше вместе с паролем");
    if (r->edit) {
        gfx_rrect(g, s, right - sw, cy - sh / 2, sw, sh, sh / 2.0f, UI_BLACK);
        gfx_rrect(g, s, right - d - 8, cy - d / 2, d, d, d / 2.0f, UI_WHITE);
    } else {
        gfx_rrect(g, s, right - sw, cy - sh / 2, sw, sh, sh / 2.0f, UI_LINE);
        gfx_rrect(g, s, right - sw + 8, cy - d / 2, d, d, d / 2.0f, UI_WHITE);
    }
    const char *t = r->edit ? "Вкл" : "Выкл";
    if (!r->edit)
        vs.gray = UI_TEXT3;
    gfx_text(g, s, right - sw - 20 - gfx_text_width(g, vs, t), cy + 12, vs, t);
    r->ed_btn = (ui_rect){ x, y - 8, w, ED_H + 16 };
}

static void draw_run(recv_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = card_x(), w = s->width - 2 * UI_MARGIN, y = ui_content_top() + 16;
    int pw = r->pass[0] != 0;
    int qh = qr_height(r), qtop = y + (pw ? 264 : 224);
    int ftp_y = qtop + qh + 68;
    int btn_y = ftp_y + (r->ftp ? 52 : 0) + 40;
    int pill_w = (w - 80 - 16) / 2;
    int ed_y = btn_y + BTN_H + 28;
    int h = ed_y + ED_H + 28 - y;
    ui_card(g, s, x, y, w, h);
    draw_centered(r, s, y + 72, 38, FONT_REGULAR, UI_BLACK, "Откройте адрес на телефоне или компьютере");
    draw_centered(r, s, y + 168, 68, FONT_BOLD, UI_BLACK, r->url);
    if (pw)
        draw_centered(r, s, y + 226, 36, FONT_REGULAR, UI_TEXT2,
                      r->pass_lost ? "Прежний пароль не открылся — задайте заново" : "Логин — любой, пароль — заданный");
    draw_qr(r, s, s->width / 2, qtop);
    if (r->ftp) {
        char l1[200], l2[260];
        snprintf(l1, sizeof l1, "FTP: %s", r->ftp_url);
        const char *who = pw ? "логин любой, пароль заданный" : "любой логин и пароль";
        if (*r->ftp_dir)
            snprintf(l2, sizeof l2, "%s — файлы в папку «%s»", who, r->ftp_dir);
        else
            snprintf(l2, sizeof l2, "%s — файлы в корень библиотеки", who);
        draw_centered(r, s, ftp_y, 36, FONT_MEDIUM, UI_BLACK, l1);
        draw_centered(r, s, ftp_y + 52, 34, FONT_REGULAR, UI_TEXT2, l2);
    } else {
        draw_centered(r, s, ftp_y, 34, FONT_REGULAR, UI_TEXT2, "FTP не запустился");
    }
    r->pw_btn = (ui_rect){ x + 40, btn_y, pill_w, BTN_H };
    ui_pill(g, s, r->pw_btn.x, r->pw_btn.y, r->pw_btn.w, r->pw_btn.h, r->pass_lost ? "Пароль: введите заново" : pw ? "Пароль: задан" : "Пароль: нет", 0);
    /* «Папка: Downloads (общая) ›» — подпись длинная, поэтому текст сжимается сам */
    r->dir_btn = (ui_rect){ x + 40 + pill_w + 16, btn_y, pill_w, BTN_H };
    ui_pill(g, s, r->dir_btn.x, r->dir_btn.y, r->dir_btn.w, r->dir_btn.h, "", 0);
    {
        char lab[1200], t[1300];
        dirsel_label(r->ds, lab, sizeof lab);
        snprintf(t, sizeof t, "Папка: %s", lab);
        gfx_style fs = { FONT_MEDIUM, 34, UI_BLACK };
        gfx_text_fit_middle(g, s, r->dir_btn.x + 36, btn_y + BTN_H / 2 + 12, pill_w - 36 - 36 - 44, fs, t);
        gfx_icon(g, s, ICON_NEXT, r->dir_btn.x + pill_w - 36 - 40, btn_y + BTN_H / 2 - 20, 40, 0, UI_BLACK);
    }

    draw_edit_row(r, s, x, ed_y, w);

    int cy = y + h + CARD_GAP, bottom = ui_content_bottom(s) - CARD_GAP;
    r->card = (ui_rect){ x, cy, w, bottom - cy };
    xfer_snap *sn = malloc(sizeof *sn);
    if (!sn)
        return;
    xfer_snapshot(&r->log, sn);
    draw_received(r, s, sn);
    free(sn);
}

/* Вопрос «Остановить приём?» — карточкой по центру на очищенном фоне. */
static void draw_confirm(recv_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int x = 96, w = s->width - 192, h = 560;
    int top = ui_content_top(), y = top + (ui_content_bottom(s) - top - h) / 2 - 60;
    ui_card(g, s, x, y, w, h);
    gfx_rrect_line(g, s, x, y, w, h, UI_CARD_R, 4, UI_BLACK);
    gfx_style ts = { FONT_MEDIUM, 46, UI_BLACK }, bs = { FONT_REGULAR, 36, UI_TEXT2 };
    gfx_text(g, s, x + 48, y + 100, ts, "Остановить приём?");
    xfer_snap *sn = malloc(sizeof *sn);
    char msg[512] = "Приём остановится.";
    if (sn) {
        xfer_snapshot(&r->log, sn);
        if (sn->nactive == 1)
            snprintf(msg, sizeof msg, "Сейчас передаётся «%s». Если остановить, передача прервётся, а файл не сохранится.",
                     sn->active[0].name);
        else if (sn->nactive > 1)
            snprintf(msg, sizeof msg, "Сейчас передаётся файлов: %d. Если остановить, они не сохранятся.", sn->nactive);
        free(sn);
    }
    gfx_text_wrap(g, s, x + 48, y + 180, w - 96, 52, 5, bs, msg);
    int bw = (w - 96 - 24) / 2, by = y + h - 48 - BTN_H;
    ui_pill(g, s, x + 48, by, bw, BTN_H, "Остановить", 0);
    ui_pill(g, s, x + 48 + bw + 24, by, bw, BTN_H, "Продолжить", 1);
    r->ov_stop = (ui_rect){ x + 48, by, bw, BTN_H };
    r->ov_go = (ui_rect){ x + 48 + bw + 24, by, bw, BTN_H };
}

/* Выбор «Задать пароль» / «Убрать пароль» — карточкой по центру на очищенном фоне. */
static void draw_menu(recv_page *r, pf_screen *s)
{
    gfx *g = r->env.g;
    int pw = r->pass[0] != 0;
    int x = 96, w = s->width - 192, h = pw ? 760 : 640;
    int top = ui_content_top(), y = top + (ui_content_bottom(s) - top - h) / 2 - 60;
    ui_card(g, s, x, y, w, h);
    gfx_rrect_line(g, s, x, y, w, h, UI_CARD_R, 4, UI_BLACK);
    gfx_style ts = { FONT_MEDIUM, 46, UI_BLACK }, bs = { FONT_REGULAR, 36, UI_TEXT2 };
    gfx_text(g, s, x + 48, y + 100, ts, "Пароль для приёма");
    gfx_text_wrap(g, s, x + 48, y + 180, w - 96, 52, 3, bs,
                  r->pass_lost ? "Сохранённый пароль не открылся (другое устройство или повреждённый файл). Приём закрыт, пока не зададите новый пароль или не уберёте его."
                  : pw ? "Пароль задан. Его спросят в браузере и при входе по FTP; сам он на экране не показывается."
                     : "Сейчас приём без пароля: отправить книги может любой в этой сети.");
    int bw = w - 96, by = y + 180 + 3 * 52 + 24;
    r->m_set = (ui_rect){ x + 48, by, bw, BTN_H };
    ui_pill(g, s, r->m_set.x, r->m_set.y, bw, BTN_H, pw && !r->pass_lost ? "Сменить пароль" : "Задать пароль", 1);
    by += BTN_H + 16;
    if (pw) {
        r->m_clear = (ui_rect){ x + 48, by, bw, BTN_H };
        ui_pill(g, s, r->m_clear.x, r->m_clear.y, bw, BTN_H, "Убрать пароль", 0);
        by += BTN_H + 16;
    }
    r->m_cancel = (ui_rect){ x + 48, by, bw, BTN_H };
    ui_pill(g, s, r->m_cancel.x, r->m_cancel.y, bw, BTN_H, "Отмена", 0);
}

static void recv_draw(page *p, pf_screen *s)
{
    recv_page *r = (recv_page *)p;
    gfx_fill(s, 0, UI_STATUS_H, s->width, ui_content_bottom(s) - UI_STATUS_H, UI_BG);
    r->hits = ui_header(r->env.g, s, "Приём по Wi-Fi", 1, 0, 1);
    r->button = (ui_rect){ 0, 0, 0, 0 };
    r->ov_stop = r->ov_go = (ui_rect){ 0, 0, 0, 0 };
    r->pw_btn = r->ed_btn = r->dir_btn = r->m_set = r->m_clear = r->m_cancel = (ui_rect){ 0, 0, 0, 0 };
    r->sh_valid = 0;
    if (r->st == ST_RUN && !r->confirm && dirsel_active(r->ds)) {
        dirsel_draw(r->ds, s);
        return;
    }
    if (r->menu && r->st == ST_RUN && !r->confirm) {
        draw_menu(r, s);
        return;
    }
    r->menu = 0;
    if (r->confirm && r->st == ST_RUN) {
        draw_confirm(r, s);
        return;
    }
    r->confirm = 0;
    switch (r->st) {
    case ST_CONNECT:
        draw_message(r, s, ICON_WIFI, "Подключаюсь к Wi-Fi…", "Запускаю приём. Это займёт несколько секунд.", "Отмена");
        break;
    case ST_NOWIFI: {
        char t[300];
        snprintf(t, sizeof t, "Устройство не подключено к сети. Подключитесь к той же сети, в которой телефон или компьютер.");
        if (r->tried_at) {
            struct tm tm;
            char hm[16];
            localtime_r(&r->tried_at, &tm);
            strftime(hm, sizeof hm, "%H:%M", &tm);
            size_t n = strlen(t);
            snprintf(t + n, sizeof t - n, " Известной сети рядом нет (%s) — выберите сеть.", hm);
        }
        draw_message(r, s, ICON_WIFI_OFF, "Нет Wi-Fi", t, "Выбрать сеть");
        break;
    }
    case ST_IDLE: {
        char t[300];
        int mins = (int)(IDLE_STOP_MS / 60000.0 + 0.5);
        if (mins < 1)
            mins = 1;
        snprintf(t, sizeof t, "%d %s ничего не передавали — приём выключен, чтобы не тратить заряд.", mins,
                 ui_plural(mins, "минуту", "минуты", "минут"));
        draw_message(r, s, ICON_DOWNLOAD, "Приём выключен", t, "Начать снова");
        break;
    }
    case ST_FAIL:
        draw_message(r, s, ICON_WIFI_OFF, "Не удалось начать приём",
                     "Не получилось открыть порт для приёма. Попробуйте ещё раз.", "Повторить");
        break;
    case ST_RUN:
        draw_run(r, s);
        break;
    }
}

/* ---- события ---- */

/* Wi-Fi после приёма не выключаем: WiFiPower(0) держит книгу 2–4,5 с, а подключение заново —
 * ещё ~3 с при следующем открытии. Пусть остаётся подключённым; выключит прошивка, когда книга
 * уснёт, или человек — плиткой в шторке. */

static void recv_open(page *p)
{
    recv_page *r = (recv_page *)p;
    r->st = ST_CONNECT; /* подключаемся в первом tick, когда экран уже нарисован */
    r->connecting = 0;
    r->confirm = 0;
    r->tried_at = 0;
    r->menu = 0;
    r->open = 1;
    dirsel_load(r->ds);
    r->pass_lost = xfer_pass_load(r->pass_file, r->pass, sizeof r->pass) < 0;
    r->edit = xfer_edit_load(r->pass_file);
}

static void recv_close(page *p)
{
    recv_page *r = (recv_page *)p;
    stop_servers(r);
    r->st = ST_CONNECT;
    r->confirm = 0;
    r->menu = 0;
    r->open = 0;
    dirsel_load(r->ds); /* закрывает выбор папки */
}

/* Идёт передача — спросить «Остановить?» (ответ — recv_tap). */
static int recv_leave(page *p)
{
    recv_page *r = (recv_page *)p;
    if (r->st != ST_RUN || xfer_active_count(&r->log) == 0)
        return 0;
    r->confirm = 1;
    r->menu = 0;
    return 1;
}

/* Ответ клавиатуры: новый пароль — сохранить (зашифрованно) и сразу применить к серверам. */
static void kb_done(const char *text, void *user)
{
    recv_page *r = user;
    if (text && *text) {
        char v[sizeof r->pass];
        snprintf(v, sizeof v, "%s", text);
        v[strcspn(v, "\r\n")] = 0;
        if (*v) {
            memcpy(r->pass, v, sizeof r->pass);
            r->pass_lost = 0;
            xfer_set_password(&r->log, r->pass);
            if (xfer_pass_save(r->pass_file, r->pass) != 0)
                log_msg("приём: не удалось записать пароль");
            else
                log_msg("приём: пароль задан");
        }
        memset(v, 0, sizeof v);
    }
    if (r->open)
        r->env.redraw(r->env.user, 1); /* после экрана прошивки — полное обновление */
}

static void clear_password(recv_page *r)
{
    memset(r->pass, 0, sizeof r->pass);
    r->pass_lost = 0;
    xfer_set_password(&r->log, NULL);
    if (xfer_pass_save(r->pass_file, "") != 0)
        log_msg("приём: не удалось убрать пароль из файла");
    else
        log_msg("приём: пароль убран");
}

static int recv_tap(page *p, int x, int y)
{
    recv_page *r = (recv_page *)p;
    pf_screen *s = pf_get_screen();
    if (r->confirm) {
        if (ui_inside(r->ov_stop, x, y)) {
            if (s)
                ui_flash(r->env.g, s, r->ov_stop);
            return PAGE_CLOSE;
        }
        if (ui_inside(r->ov_go, x, y)) {
            r->confirm = 0;
            return PAGE_REDRAW;
        }
        return 0;
    }
    if (r->st == ST_RUN && dirsel_active(r->ds)) {
        switch (dirsel_tap(r->ds, x, y)) {
        case DS_CHANGED:
            /* Другая папка — серверы начинаются заново (передач сейчас нет). */
            stop_servers(r);
            r->st = ST_CONNECT;
            return PAGE_REDRAW;
        case DS_REDRAW: return PAGE_REDRAW;
        default: return 0;
        }
    }
    int back = ui_inside(r->hits.back, x, y);
    if (r->menu) {
        if (back || ui_inside(r->m_cancel, x, y)) {
            r->menu = 0;
            return PAGE_REDRAW;
        }
        if (ui_inside(r->m_clear, x, y)) {
            if (s)
                ui_flash(r->env.g, s, r->m_clear);
            clear_password(r);
            r->menu = 0;
            return PAGE_REDRAW;
        }
        if (ui_inside(r->m_set, x, y)) {
            if (s)
                ui_flash(r->env.g, s, r->m_set);
            r->menu = 0;
            pf_keyboard("Пароль для приёма", "", 120, kb_done, r);
            return 0;
        }
        return 0;
    }
    if (r->st == ST_RUN && ui_inside(r->pw_btn, x, y)) {
        if (s)
            ui_flash(r->env.g, s, r->pw_btn);
        r->menu = 1;
        return PAGE_REDRAW;
    }
    if (r->st == ST_RUN && ui_inside(r->ed_btn, x, y)) {
        r->edit = !r->edit;
        xfer_set_allow_edit(&r->log, r->edit);
        if (xfer_edit_save(r->pass_file, r->edit) != 0)
            log_msg("приём: не удалось записать настройку");
        log_msg("приём: удаление и переименование из браузера %s", r->edit ? "разрешены" : "запрещены");
        return PAGE_REDRAW;
    }
    if (r->st == ST_RUN && ui_inside(r->dir_btn, x, y)) {
        if (xfer_active_count(&r->log) > 0)
            return 0; /* пока идёт передача, папку не меняют */
        if (s)
            ui_flash(r->env.g, s, r->dir_btn);
        dirsel_start(r->ds);
        return PAGE_REDRAW;
    }
    if (!back && !ui_inside(r->button, x, y))
        return 0;
    if (!back && s)
        ui_flash(r->env.g, s, r->button);
    if (back || r->st == ST_RUN) {
        if (recv_leave(p))
            return PAGE_REDRAW;
        return PAGE_CLOSE;
    }
    if (r->st == ST_CONNECT)
        return PAGE_CLOSE; /* «Отмена»: не ждать сеть (прошивка может подключиться и сама) */
    if (r->st == ST_NOWIFI) {
        choose_network(r);
        return 0;
    }
    r->st = ST_CONNECT;
    r->connecting = 0;
    return PAGE_REDRAW;
}

static int recv_key(page *p, pf_key key)
{
    (void)p;
    (void)key;
    return 0;
}

static int recv_tick(page *p, pf_screen *s)
{
    recv_page *r = (recv_page *)p;
    if (r->st == ST_CONNECT) {
        if (!r->connecting)
            return begin(r) ? PAGE_REDRAW : 0;
        int rc = pf_net_poll();
        if (rc == 0)
            return 0;
        launch(r, rc == 1 ? 0 : -1);
        return PAGE_REDRAW;
    }
    if (r->st != ST_RUN) {
        if (dirsel_active(r->ds))
            dirsel_load(r->ds);
        return 0;
    }

    double now = clock_ms();
    int flags = 0;
    if (now - r->awake_ms >= AWAKE_EVERY_MS) {
        pf_keep_awake(AWAKE_SEC);
        r->awake_ms = now;
    }
    xfer_snap *sn = malloc(sizeof *sn);
    if (!sn)
        return 0;
    xfer_snapshot(&r->log, sn);
    if (sn->books_gen != r->books_gen) {
        r->books_gen = sn->books_gen;
        flags |= PAGE_BOOKS;
    }
    if (!sn->nactive && now - sn->last_activity_ms >= IDLE_STOP_MS) {
        log_msg("приём: остановлен — нет передач %d мин", (int)(IDLE_STOP_MS / 60000.0 + 0.5));
        stop_servers(r);
        r->st = ST_IDLE;
        free(sn);
        return flags | PAGE_REDRAW;
    }
    if (now - r->ip_ms >= IP_CHECK_MS) {
        r->ip_ms = now;
        char ip[64];
        if (net_local_ip(ip, sizeof ip) != 0) {
            log_msg("приём: Wi-Fi пропал");
            stop_servers(r);
            r->st = ST_NOWIFI;
            free(sn);
            return flags | PAGE_REDRAW;
        }
        if (strcmp(ip, r->ip)) {
            snprintf(r->ip, sizeof r->ip, "%s", ip);
            make_urls(r);
            free(sn);
            return flags | PAGE_REDRAW;
        }
    }
    if (!r->confirm && !r->menu && r->sh_valid && !dirsel_active(r->ds)) {
        if (!same_struct(r, sn)) {
            /* Началась или кончилась передача: карточка целиком. */
            if (now - r->painted_ms >= PAINT_MIN_MS) {
                draw_received(r, s, sn);
                r->painted_ms = now;
                pf_update(r->card.x, r->card.y, r->card.w, r->card.h, PF_UPDATE_PARTIAL);
            }
        } else {
            paint_progress(r, s, sn, now);
        }
    }
    free(sn);
    return flags;
}

static const char *recv_selected(page *p)
{
    (void)p;
    return NULL;
}

static void recv_free(page *p)
{
    recv_page *r = (recv_page *)p;
    stop_servers(r);
    xfer_destroy(&r->log);
    dirsel_free(r->ds);
    memset(r->pass, 0, sizeof r->pass);
    free(r);
}

static const page_ops ops = { recv_open, recv_close, recv_draw, recv_tap, recv_key, recv_tick, recv_selected, recv_free,
                              recv_leave };

page *recv_page_new(const page_env *env)
{
    recv_page *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->base.ops = &ops;
    r->base.gray = 1;
    r->env = *env;
    snprintf(r->pass_file, sizeof r->pass_file, "%s/recv.txt", env->data_dir);
    char dir_file[1200];
    snprintf(dir_file, sizeof dir_file, "%s/recv_dir.txt", env->data_dir);
    r->ds = dirsel_new(env->g, env->library_root, env->data_dir, env->download_dir, dir_file, env->redraw, env->user);
    if (!r->ds) {
        free(r);
        return NULL;
    }
    xfer_init(&r->log);
    return &r->base;
}
