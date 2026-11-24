#include "reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/bookmarks.h"
#include "../base/clock.h"
#include "../base/log.h"
#include "../cache/cache.h"
#include "../doc/margins.h"
#include "../render/render.h"
#include "gesture.h"
#include "home.h"
#include "text.h"
#include "widgets.h"

#define MARGIN 60
#define TITLE_SIZE 52
#define BODY_SIZE 38

/* Нужная страница ещё рисуется: проверять часто. Рисуются только соседние: редко. */
#define POLL_WAITING_MS 15
#define POLL_PREFETCH_MS 500
#define KEEP_AWAKE_SEC 30
#define KEEP_AWAKE_EVERY_MS 10000
#define MAX_COLLECT 16
#define MAX_WANTED 16
#define MAX_RENDER_THREADS 4

/* Страница выше экрана («по ширине») листается частями, соседние части перекрываются. */
#define PART_OVERLAP_DIV 12

/* Меню и клавиатура поверх страницы (поля и оттенки — как в хабе, widgets.h). */
#define UI_TEXT 38
#define UI_ROW 116
#define MAX_CONTROLS 48

/* Поверх страницы: панель (верх и низ экрана), листы «Вид» и «Подсветка» снизу, клавиатура
 * номера страницы, содержание и закладки, подсказка по жестам при первом открытии книги. */
typedef enum {
    OVERLAY_NONE,
    OVERLAY_PANEL,
    OVERLAY_VIEW,
    OVERLAY_LIGHT,
    OVERLAY_KEYPAD,
    OVERLAY_TOC,
    OVERLAY_HINT
} overlay_kind;

typedef enum {
    ACT_GOTO,
    ACT_LIBRARY,
    ACT_CLOSE,
    ACT_VIEW,     /* масштаб: value — view_mode */
    ACT_LAYOUT,   /* расположение: 0 книжная, 1 альбомная, 2 разворот */
    ACT_CONTRAST, /* value — 0..3 */
    ACT_SLIDER,
    ACT_TOC,
    ACT_TOC_ITEM,
    ACT_TOC_PREV,
    ACT_TOC_NEXT,
    ACT_TOC_TAB, /* 0 — содержание, 1 — закладки */
    ACT_BM_ITEM, /* value — страница закладки */
    ACT_BOOKMARK,
    ACT_VIEW_SHEET,
    ACT_LIGHT_SHEET,
    ACT_LIGHT_B, /* ползунки подсветки (перетаскиваются) */
    ACT_LIGHT_W,
    ACT_LIGHT_OFF,
    ACT_BACK_JUMP, /* вернуться, откуда прыгнули */
    ACT_DIGIT,
    ACT_BACKSPACE,
    ACT_OK,
    ACT_CANCEL,
} action;

/* Зоны касания страницы: верхняя полоса — панель (её правый угол — закладка), левая треть —
 * назад, остальное — вперёд; вертикальный свайп у левого края — подсветка. */
#define TOP_ZONE_DIV 8
#define CORNER 200     /* правый верхний угол — закладка */
#define EDGE_DIV 5       /* полосы у краёв (1/5 ширины): левая — яркость, правая — теплота */
#define EDGE_START 40   /* столько провести вдоль края, чтобы жест стал подсветкой */
#define EDGE_RANGE_DIV 2 /* весь диапазон 0–100 % — на 1/2 высоты экрана */
#define JUMP_PAGES 10  /* удержание кнопки листания — через столько страниц */
#define PANEL_TOP_H (UI_STATUS_H + 124)
#define PANEL_BOTTOM_H 360
#define TOC_ROW_H 118
#define SHEET_SEG_H 100

typedef struct {
    int x, y, w, h;
} box;

typedef struct {
    box b;
    action act;
    int value;
} control;

struct reader {
    reader_options opt;
    char path[1024];
    renderer *render;
    cache *cache;
    ui_text *text;
    int page_count;

    int target;        /* страница, которую нужно показать */
    int target_offset; /* с какой строки её картинки показывать; -1 — нижняя часть */
    int shown;         /* страница на экране, -1 — никакой */
    int offset;        /* строка картинки shown в верху экрана */
    int waiting;       /* target ещё не показана: не готова или экран занят */
    int wait_cached;   /* target была в кэше уже при нажатии */
    double wait_since; /* время события, с которого ждём target */
    double busy_until; /* до этого времени экран занят прошлым обновлением */
    int need_full;     /* следующий показ — полным обновлением */
    int turns_since_full;
    double last_keep_awake;
    int paused;   /* на экране библиотека */
    int requests; /* READER_* для программы */
    char error[512];

    overlay_kind overlay;
    char keypad[8];
    control ctl[MAX_CONTROLS];
    int nctl;
    int panel_h; /* высота панели клавиатуры (её область обновляется) */
    gesture_state gs;

    int dragging;          /* ведут ползунок: ACT_SLIDER, ACT_LIGHT_B или ACT_LIGHT_W; 0 — нет */
    int drag_page;         /* страница под ползунком */
    double drag_drawn;     /* когда ползунок последний раз показан */
    box drag_box;          /* область ползунка подсветки */

    doc *udoc;             /* своя копия документа для оглавления и ссылок (в потоке интерфейса) */
    doc_outline_item *toc; /* оглавление, загружается при первом показе панели */
    int ntoc;
    int toc_loaded;
    int toc_page, toc_pages, toc_rows;
    int toc_tab; /* 0 — содержание, 1 — закладки */

    bookmarks bm;
    doc_link *links;  /* ссылки показанной картинки, в координатах экрана (rect) */
    int nlinks;
    int links_for;    /* для какой показанной страницы (shown), -1 — не загружены */
    int links_offset; /* при каком offset */

    int back_page;  /* откуда прыгнули (содержание, ползунок, ссылка…); -1 — нет */
    int chip;       /* показывать чип «← стр.» на странице */
    box chip_box;

    int light_b, light_w; /* подсветка: яркость, теплота (-1 — нет) */

    /* Подсветка жестом вдоль края: палец у края (1 — левого, яркость; 2 — правого, теплота),
     * жест начат (ведут вверх-вниз), откуда начали, значение в начале и сейчас. */
    int edge, edge_active;
    int edge_x0, edge_y0, edge_v0, edge_v;
    double edge_drawn;
    int edge_sets;
    double edge_set_ms;
    box edge_box; /* где нарисован указатель уровня */
};

reader *reader_new(const reader_options *opt)
{
    reader *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->opt = *opt;
    if (opt->path) {
        snprintf(r->path, sizeof r->path, "%s", opt->path);
        r->opt.path = r->path;
    }
    if (r->opt.view == VIEW_CROP && !r->opt.has_crop)
        r->opt.view = VIEW_FIT_PAGE;
    if (r->opt.spread && !r->opt.orient)
        r->opt.spread = 0; /* разворот — только в альбомной */
    r->shown = -1;
    r->need_full = 1;
    r->back_page = -1;
    r->links_for = -1;
    char file[1100];
    snprintf(file, sizeof file, "%s/bookmarks.txt", pf_data_dir());
    bookmarks_load(&r->bm, file, r->path);
    return r;
}

void reader_free(reader *r)
{
    if (!r)
        return;
    free(r->toc);
    free(r->links);
    doc_close(r->udoc);
    renderer_free(r->render);
    cache_free(r->cache);
    ui_text_free(r->text);
    free(r);
}

static ui_text *text_of(reader *r)
{
    if (!r->text)
        r->text = ui_text_new();
    return r->text;
}

static void show_message(reader *r, const char *title, const char *body)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    ui_fill(s, 0, 0, s->width, s->height, 255);
    int y = MARGIN + TITLE_SIZE;
    y = ui_text_draw_wrapped(r->text, s, MARGIN, y, s->width - 2 * MARGIN, TITLE_SIZE, 0, title);
    ui_text_draw_wrapped(r->text, s, MARGIN, y + BODY_SIZE, s->width - 2 * MARGIN, BODY_SIZE, 0, body);
    pf_update(0, 0, s->width, s->height, PF_UPDATE_FULL_HQ);
}

static view_settings current_view(const reader *r)
{
    view_settings v;
    memset(&v, 0, sizeof v);
    v.mode = (view_mode)r->opt.view;
    v.spread = r->opt.spread;
    v.contrast = r->opt.contrast;
    for (int k = 0; k < 2; k++) {
        const float *c = r->opt.crop + 4 * k;
        v.crop[k].x0 = c[0];
        v.crop[k].y0 = c[1];
        v.crop[k].x1 = c[2];
        v.crop[k].y1 = c[3];
    }
    return v;
}

static void open_document(reader *r)
{
    if (!r->opt.path) {
        snprintf(r->error, sizeof r->error,
                 "Укажите PDF в %s/config.txt строкой file=/mnt/ext1/путь/к/файлу.pdf",
                 pf_data_dir());
        log_msg("файл не указан");
        return;
    }
    pf_screen *s = pf_get_screen();
    if (!s) {
        snprintf(r->error, sizeof r->error, "Экран не в формате 8 бит серого");
        log_msg("экран не 8 бит серого");
        return;
    }
    char err[256];
    double t0 = clock_ms();
    /* «Перетекающие» книги (EPUB/FB2/HTML) верстаются под размер экрана в пунктах:
     * страница в масштабе 1 занимает экран целиком — без увеличения и обрезки.
     * Кегль — из настроек («Размер шрифта»). PDF-путь не меняется (reflow_* для него
     * игнорируются). */
    r->opt.doc.reflow_w = s->width;
    r->opt.doc.reflow_h = s->height;
    r->opt.doc.reflow_em = r->opt.reflow_em;
    doc *docs[MAX_RENDER_THREADS];
    docs[0] = doc_open(r->opt.path, &r->opt.doc, err, sizeof err);
    if (!docs[0]) {
        snprintf(r->error, sizeof r->error, "%s\n%s", r->opt.path, err);
        log_msg("open %s: ошибка: %s", r->opt.path, err);
        return;
    }
    r->page_count = doc_page_count(docs[0]);
    log_msg("open %s: %.1f ms, %d стр.%s", r->opt.path, clock_ms() - t0, r->page_count,
            doc_reflowable(docs[0]) ? " (перетекающая, вёрстка под экран)" : "");

    /* По копии документа на поток отрисовки; дальше копиями владеют потоки. */
    int n = 1;
    int want = r->opt.render_threads < 1                    ? 1
               : r->opt.render_threads > MAX_RENDER_THREADS ? MAX_RENDER_THREADS
                                                            : r->opt.render_threads;
    if (doc_reflowable(docs[0]))
        want = 1; /* каждая копия — отдельная вёрстка всей книги в потоке интерфейса (секунды на книге) */
    for (; n < want; n++) {
        docs[n] = doc_open(r->opt.path, &r->opt.doc, err, sizeof err);
        if (!docs[n]) {
            log_msg("копия документа %d: %s — потоков будет %d", n, err, n);
            break;
        }
    }
    r->cache = cache_new(r->opt.cache_bytes);
    r->render = r->cache ? renderer_new(docs, n, s->width, s->height, s->stride) : NULL;
    if (!r->render) {
        if (!r->cache)
            for (int i = 0; i < n; i++)
                doc_close(docs[i]);
        snprintf(r->error, sizeof r->error, "Не удалось запустить поток отрисовки");
        log_msg("renderer_new не удался");
        return;
    }
    view_settings v = current_view(r);
    renderer_set_view(r->render, &v);
    renderer_set_urgent_alone(r->render, !r->opt.prefetch_while_waiting);
    log_msg("потоков отрисовки: %d%s; масштаб %d, ориентация %d, разворот %d, контраст %d", n,
            r->opt.prefetch_while_waiting ? "" : ", нужная страница рисуется одна", r->opt.view, r->opt.orient,
            r->opt.spread, r->opt.contrast);
    char info[256];
    pf_screen_info(info, sizeof info);
    log_msg("экран: %s", info);
}

/* Забрать готовые страницы у потока в кэш. */
static void collect(reader *r)
{
    page_image *imgs[MAX_COLLECT];
    int n = renderer_collect(r->render, imgs, MAX_COLLECT);
    for (int i = 0; i < n; i++)
        cache_put(r->cache, imgs[i]);
    if (n > 0)
        log_msg("кэш: +%d, всего %d стр., %.1f MB", n, cache_count(r->cache),
                cache_bytes(r->cache) / 1048576.0);
}

/* Шаг листания: в развороте — по две страницы, картинка разворота — по чётной (левой). */
static int pstep(const reader *r)
{
    return r->opt.spread ? 2 : 1;
}

static int align(const reader *r, int page)
{
    return r->opt.spread ? page & ~1 : page;
}

/* Что рисовать: N, N+1, N−1, N+2, … в пределах prefetch_ahead/behind, кроме того, что в кэше. */
static void schedule(reader *r)
{
    int pages[MAX_WANTED];
    int n = 0;
    int t = r->target, k1 = pstep(r);
    int depth = r->opt.prefetch_ahead > r->opt.prefetch_behind ? r->opt.prefetch_ahead
                                                                : r->opt.prefetch_behind;
    int candidates[MAX_WANTED];
    int c = 0;
    candidates[c++] = t;
    for (int k = 1; k <= depth && c < MAX_WANTED - 1; k++) {
        if (k <= r->opt.prefetch_ahead)
            candidates[c++] = t + k * k1;
        if (k <= r->opt.prefetch_behind)
            candidates[c++] = t - k * k1;
    }
    for (int i = 0; i < c; i++)
        if (candidates[i] >= 0 && candidates[i] < r->page_count && !cache_has(r->cache, candidates[i]))
            pages[n++] = candidates[i];

    cache_protect(r->cache, t - r->opt.prefetch_behind * k1, t + r->opt.prefetch_ahead * k1);
    renderer_request(r->render, pages, n, !cache_has(r->cache, t));
}

static void arm_timer(reader *r)
{
    double now = clock_ms();
    if (r->waiting && cache_has(r->cache, r->target) && r->busy_until > now)
        pf_set_timer((int)(r->busy_until - now) + 1);
    else if (r->waiting)
        pf_set_timer(POLL_WAITING_MS);
    else if (renderer_busy(r->render))
        pf_set_timer(POLL_PREFETCH_MS);
}

static void keep_awake(reader *r)
{
    double now = clock_ms();
    if (now - r->last_keep_awake >= KEEP_AWAKE_EVERY_MS) {
        pf_keep_awake(KEEP_AWAKE_SEC);
        r->last_keep_awake = now;
    }
}

/* Окно картинки страницы с её строки offset — на экран. */
static double blit(const page_image *img, int offset, pf_screen *s)
{
    double t0 = clock_ms();
    int h = img->height - offset < s->height ? img->height - offset : s->height;
    if (img->stride == s->stride && img->width == s->width && h == s->height) {
        memcpy(s->pixels, img->pixels + (size_t)offset * img->stride, (size_t)img->stride * h);
    } else {
        int w = img->width < s->width ? img->width : s->width;
        for (int y = 0; y < h; y++)
            memcpy(s->pixels + (size_t)y * s->stride, img->pixels + (size_t)(y + offset) * img->stride, (size_t)w);
        if (h < s->height)
            ui_fill(s, 0, h, s->width, s->height - h, 255);
    }
    return clock_ms() - t0;
}

static int max_offset(const page_image *img, const pf_screen *s)
{
    return img->height > s->height ? img->height - s->height : 0;
}

static void draw_hint(reader *r);

/* Лента закладки в правом верхнем углу (с белой обводкой — видна и на тёмной странице). */
#define RIBBON "M0 0H40V58L20 45L0 58Z"
static void draw_ribbon(reader *r, pf_screen *s)
{
    static const float vb[4] = { 0, 0, 40, 58 };
    int size = 116, x = s->width - 60 - size / 2 - 38, y = -8;
    gfx_svg(r->text, s, RIBBON, vb, x, y, size, 8, UI_WHITE);
    gfx_svg(r->text, s, RIBBON, vb, x, y, size, 0, UI_BLACK);
}

static int page_bookmarked(const reader *r, int page)
{
    return bookmarks_has(&r->bm, page) || (r->opt.spread && bookmarks_has(&r->bm, page + 1));
}

/* Чип «← стр. N» внизу слева: вернуться, откуда прыгнули. */
static void draw_chip(reader *r, pf_screen *s)
{
    char label[48];
    snprintf(label, sizeof label, "←  стр. %d", r->back_page + 1);
    gfx_style st = { FONT_MEDIUM, 34, UI_WHITE };
    int w = gfx_text_width(r->text, st, label) + 72, h = 84, x = UI_MARGIN, y = s->height - h - 48;
    gfx_rrect(r->text, s, x - 4, y - 4, w + 8, h + 8, (h + 8) / 2.0f, UI_WHITE);
    gfx_rrect(r->text, s, x, y, w, h, h / 2.0f, UI_BLACK);
    gfx_text(r->text, s, x + 36, y + h / 2 + 12, st, label);
    r->chip_box = (box){ x - 20, y - 20, w + 40, h + 40 };
}

/* Знаки поверх страницы: закладка и чип возврата. */
static void draw_marks(reader *r, pf_screen *s)
{
    if (!text_of(r))
        return;
    if (page_bookmarked(r, r->target))
        draw_ribbon(r, s);
    memset(&r->chip_box, 0, sizeof r->chip_box);
    if (r->chip && r->back_page >= 0 && r->back_page != r->target)
        draw_chip(r, s);
}

/* Показать r->target из кэша. t_input — событие, с которого считается задержка. */
static void show_target(reader *r, double t_input, int from_cache)
{
    pf_screen *s = pf_get_screen();
    page_image *img = cache_get(r->cache, r->target);
    if (!s || !img)
        return;
    int maxoff = max_offset(img, s);
    int off = r->target_offset < 0 || r->target_offset > maxoff ? maxoff : r->target_offset;
    if (r->opt.show_hint) {
        /* Первая книга: сначала подсказка по жестам, страница — после касания. */
        r->opt.show_hint = 0;
        r->requests |= READER_HINT_SHOWN;
        r->shown = r->target;
        r->offset = r->target_offset = off;
        r->waiting = 0;
        r->need_full = 1;
        r->overlay = OVERLAY_HINT;
        draw_hint(r);
        return;
    }
    double t0 = clock_ms();
    double copy_ms = blit(img, off, s);
    draw_marks(r, s);

    pf_update_mode mode = PF_UPDATE_PARTIAL;
    r->turns_since_full++;
    if (r->need_full ||
        (r->opt.full_refresh_every > 0 && r->turns_since_full >= r->opt.full_refresh_every)) {
        mode = PF_UPDATE_FULL_HQ; /* на книге на 140–170 мс быстрее FullUpdate (тест экрана) */
        r->turns_since_full = 0;
        r->need_full = 0;
    }
    double t_update = clock_ms();
    pf_update(0, 0, s->width, s->height, mode);
    double t_done = clock_ms();
    r->busy_until = t_done + r->opt.screen_busy_ms;

    r->shown = r->target;
    r->offset = r->target_offset = off;
    r->waiting = 0;
    if (r->target + pstep(r) >= r->page_count)
        r->requests |= READER_FINISHED;
    size_t heap;
    doc_mem_stats(&heap, NULL);
    char part[48] = "";
    if (img->height > s->height)
        snprintf(part, sizeof part, " (с %d из %d строк)", off, img->height);
    log_msg("показ стр. %d/%d%s: %s, копия %.1f, %s %.1f ms; ввод→обновление %.1f ms (ожидание %.1f)%s; "
            "кэш %d стр. %.1f MB, heap %.1f MB",
            r->target + 1, r->page_count, part, from_cache ? "из кэша" : "дождались", copy_ms,
            pf_update_mode_name(mode), t_done - t_update, t_update - t_input, t0 - t_input,
            img->failed ? ", ОШИБКА СТРАНИЦЫ" : "", cache_count(r->cache),
            cache_bytes(r->cache) / 1048576.0, heap / 1048576.0);
}

/* Перейти к странице page, к части с её строки offset (-1 — нижняя часть). */
static void go_to(reader *r, int page, int offset, double t_input)
{
    page = align(r, page);
    if (page < 0 || page >= r->page_count) {
        log_msg("листание за край: стр. %d", page + 1);
        return;
    }
    collect(r);
    r->target = page;
    r->target_offset = offset;
    r->wait_since = t_input;
    r->wait_cached = cache_has(r->cache, page);
    double busy = r->busy_until - clock_ms();
    if (r->wait_cached && busy <= 0) {
        log_msg("→ стр. %d: в кэше", page + 1);
        show_target(r, t_input, 1);
    } else if (r->wait_cached) {
        /* Экран ещё показывает прошлую страницу: следующие нажатия сдвинут цель,
         * а покажется та, на которой остановились. */
        log_msg("→ стр. %d: в кэше, экран занят ещё %.0f ms", page + 1, busy);
        r->waiting = 1;
    } else {
        log_msg("→ стр. %d: нет в кэше, ждём", page + 1);
        r->waiting = 1;
    }
    schedule(r);
    keep_awake(r);
    arm_timer(r);
}

/* Вперёд: следующая часть страницы выше экрана или следующая страница. */
static void step(reader *r, int dir, double t)
{
    pf_screen *s = pf_get_screen();
    page_image *img = !r->waiting && r->shown == r->target ? cache_get(r->cache, r->target) : NULL;
    if (s && img && img->height > s->height) {
        int stepped = s->height - s->height / PART_OVERLAP_DIV;
        int maxoff = max_offset(img, s);
        if (dir > 0 && r->offset < maxoff) {
            go_to(r, r->target, r->offset + stepped < maxoff ? r->offset + stepped : maxoff, t);
            return;
        }
        if (dir < 0 && r->offset > 0) {
            go_to(r, r->target, r->offset - stepped > 0 ? r->offset - stepped : 0, t);
            return;
        }
    }
    /* Листают дальше — значит, остаются здесь: возврат больше не нужен (и в панели тоже). */
    r->chip = 0;
    r->back_page = -1;
    go_to(r, r->target + dir * pstep(r), dir > 0 ? 0 : -1, t);
}

static void on_timer(reader *r)
{
    collect(r);
    if (r->paused || r->overlay != OVERLAY_NONE)
        return;
    if (r->waiting && cache_has(r->cache, r->target) && clock_ms() >= r->busy_until)
        show_target(r, r->wait_since, r->wait_cached);
    if (renderer_busy(r->render))
        keep_awake(r);
    arm_timer(r);
}

static void on_show(reader *r, double t)
{
    r->need_full = 1;
    if (r->waiting)
        return; /* покажем полным обновлением, когда дорисуется */
    if (r->shown >= 0 && cache_has(r->cache, r->shown)) {
        r->target = r->shown;
        r->target_offset = r->offset;
        show_target(r, t, 1);
    } else {
        go_to(r, r->shown >= 0 ? r->shown : 0, 0, t);
    }
}

/* ---- Панель, листы, содержание ---- */

static void add_control(reader *r, box b, action act, int value)
{
    if (r->nctl < MAX_CONTROLS) {
        r->ctl[r->nctl].b = b;
        r->ctl[r->nctl].act = act;
        r->ctl[r->nctl].value = value;
        r->nctl++;
    }
}

/* Кнопка: скруглённая рамка, выбранная — залита чёрным, надпись по центру. */
static void button(reader *r, pf_screen *s, box b, const char *label, int selected, action act, int value, float size)
{
    gfx_fill(s, b.x, b.y, b.w, b.h, UI_WHITE);
    if (selected)
        gfx_rrect(r->text, s, b.x, b.y, b.w, b.h, 22, UI_BLACK);
    else
        gfx_rrect_line(r->text, s, b.x, b.y, b.w, b.h, 22, 2.5f, 0xaa);
    gfx_style st = { selected ? FONT_MEDIUM : FONT_REGULAR, size, selected ? UI_WHITE : UI_BLACK };
    int tw = gfx_text_width(r->text, st, label);
    int x = b.x + (b.w - tw) / 2;
    gfx_text_fit(r->text, s, x > b.x + 16 ? x : b.x + 16, b.y + b.h / 2 + (int)(size * 0.36f), b.w - 32, st, label);
    add_control(r, b, act, value);
}

/* Ряд сегментов на всю ширину: выбран тот, у которого values[i] == cur. */
static void segments(reader *r, pf_screen *s, int y, const char *const *labels, const int *values, int n, int cur,
                     action act)
{
    int gap = 14, w = (s->width - 2 * UI_MARGIN - gap * (n - 1)) / n;
    for (int i = 0; i < n; i++)
        button(r, s, (box){ UI_MARGIN + i * (w + gap), y, w, SHEET_SEG_H }, labels[i], values[i] == cur, act, values[i],
               34);
}

/* Своя копия документа для оглавления и ссылок (открывается при первой надобности). */
static doc *udoc(reader *r)
{
    if (!r->udoc) {
        char err[256];
        doc_options opt = r->opt.doc;
        opt.store_bytes = (size_t)8 << 20;
        r->udoc = doc_open(r->opt.path, &opt, err, sizeof err);
        if (!r->udoc)
            log_msg("копия документа для ссылок: %s", err);
    }
    return r->udoc;
}

static void load_toc(reader *r)
{
    if (r->toc_loaded)
        return;
    r->toc_loaded = 1;
    double t0 = clock_ms();
    doc *d = udoc(r);
    if (d)
        r->ntoc = doc_outline(d, &r->toc);
    log_msg("оглавление: %d пунктов, %.0f ms", r->ntoc, clock_ms() - t0);
}

/* Пункт оглавления, в котором страница page (последний с началом не дальше page); -1 — нет. */
static int toc_current(const reader *r, int page)
{
    int best = -1;
    for (int i = 0; i < r->ntoc; i++)
        if (r->toc[i].page >= 0 && r->toc[i].page <= page)
            best = i;
    return best;
}

/* ---- Ползунок: глава и «стр. из N» над ним ---- */

#define SLIDER_ROWS_H 200

static int slider_page(const reader *r, const pf_screen *s, int x)
{
    int tw, tx = ui_slider_x(s, &tw);
    float f = (float)(x - tx) / tw;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    return (int)(f * (r->page_count - 1) + 0.5f);
}

static void draw_slider(reader *r, pf_screen *s, int page)
{
    gfx *g = r->text;
    int W = s->width, M = UI_MARGIN, y0 = s->height - PANEL_BOTTOM_H;
    gfx_fill(s, 0, y0 + 2, W, SLIDER_ROWS_H - 2, UI_WHITE);
    char where[64];
    if (r->opt.spread && page + 1 < r->page_count)
        snprintf(where, sizeof where, "%d–%d из %d", page + 1, page + 2, r->page_count);
    else
        snprintf(where, sizeof where, "%d из %d", page + 1, r->page_count);
    gfx_style ws = { FONT_MEDIUM, 36, UI_BLACK }, cs = { FONT_REGULAR, 33, UI_TEXT2 };
    int ww = gfx_text_width(g, ws, where);
    gfx_text(g, s, W - M - ww, y0 + 72, ws, where);
    int cur = toc_current(r, page);
    gfx_text_fit(g, s, M, y0 + 72, W - 2 * M - ww - 40, cs, cur >= 0 ? r->toc[cur].title : "");
    int tw, tx = ui_slider_x(s, &tw);
    ui_slider_track(g, s, tx, y0 + 140, tw, r->page_count > 1 ? (float)page / (r->page_count - 1) : 0);
}

/* Панель: сверху — время, заряд, «‹ книга» (в хаб), подсветка и закладка; снизу — глава,
 * страница, ползунок, «Содержание», «Перейти», «Вид» (и «← стр.», если прыгали).
 * Страница между ними остаётся видна. */
static void draw_panel(reader *r)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    double t0 = clock_ms();
    load_toc(r);
    gfx *g = r->text;
    int W = s->width, H = s->height, M = UI_MARGIN;
    r->nctl = 0;

    /* Верх. */
    gfx_fill(s, 0, 0, W, PANEL_TOP_H, UI_WHITE);
    ui_status(g, s);
    int base = UI_STATUS_H + 78, isz = 64;
    int bx = W - M - isz, lx = bx - isz - 48;
    gfx_icon_ex(g, s, ICON_BOOKMARK, bx, base - 52, isz, 0, page_bookmarked(r, r->target), UI_BLACK);
    add_control(r, (box){ bx - 20, UI_STATUS_H, isz + 40 + M, PANEL_TOP_H - UI_STATUS_H }, ACT_BOOKMARK, 0);
    if (r->light_b >= 0) {
        gfx_icon(g, s, ICON_SUN, lx, base - 52, isz, 0, UI_BLACK);
        add_control(r, (box){ lx - 20, UI_STATUS_H, isz + 40, PANEL_TOP_H - UI_STATUS_H }, ACT_LIGHT_SHEET, 0);
    }
    gfx_icon(g, s, ICON_BACK, M - 14, base - 50, 60, 0, UI_BLACK);
    char name[512];
    home_book_title(r->opt.path, name, sizeof name);
    gfx_style ts = { FONT_MEDIUM, 42, UI_BLACK };
    gfx_text_fit(g, s, M + 64, base, lx - 40 - M - 64, ts, name);
    gfx_fill(s, 0, PANEL_TOP_H - 2, W, 2, UI_TEXT3);
    add_control(r, (box){ 0, UI_STATUS_H, lx - 20, PANEL_TOP_H - UI_STATUS_H }, ACT_LIBRARY, 0);

    /* Низ. */
    int y0 = H - PANEL_BOTTOM_H;
    gfx_fill(s, 0, y0, W, PANEL_BOTTOM_H, UI_WHITE);
    gfx_fill(s, 0, y0, W, 2, UI_TEXT3);
    draw_slider(r, s, r->target);
    add_control(r, (box){ 0, y0 + 85, W, 110 }, ACT_SLIDER, 0);

    int n = r->back_page >= 0 ? 4 : 3, gap = 16, bw = (W - 2 * M - gap * (n - 1)) / n, by = y0 + 220;
    int x = M;
    if (r->back_page >= 0) {
        char back[32];
        snprintf(back, sizeof back, "← стр. %d", r->back_page + 1);
        button(r, s, (box){ x, by, bw, 104 }, back, 1, ACT_BACK_JUMP, 0, 34);
        x += bw + gap;
    }
    button(r, s, (box){ x, by, bw, 104 }, "Содержание", 0, ACT_TOC, 0, 34);
    button(r, s, (box){ x + bw + gap, by, bw, 104 }, "Перейти", 0, ACT_GOTO, 0, 34);
    button(r, s, (box){ x + 2 * (bw + gap), by, bw, 104 }, "Вид", 0, ACT_VIEW_SHEET, 0, 34);

    double t1 = clock_ms();
    pf_update(0, 0, W, PANEL_TOP_H, PF_UPDATE_PARTIAL);
    pf_update(0, y0, W, PANEL_BOTTOM_H, PF_UPDATE_PARTIAL);
    log_msg("панель: отрисовка %.1f ms", t1 - t0);
}

/* Лист снизу: белый, с чертой сверху и заголовком; возвращает y под заголовком. */
static int sheet_start(reader *r, pf_screen *s, int h, const char *title)
{
    int y0 = s->height - h;
    r->nctl = 0;
    r->panel_h = h;
    gfx_fill(s, 0, y0, s->width, h, UI_WHITE);
    gfx_fill(s, 0, y0, s->width, 2, UI_TEXT3);
    gfx_style ts = { FONT_BOLD, 44, UI_BLACK };
    gfx_text(r->text, s, UI_MARGIN, y0 + 74, ts, title);
    return y0 + 104;
}

static void sheet_done(reader *r, pf_screen *s, int y)
{
    button(r, s, (box){ UI_MARGIN, y, s->width - 2 * UI_MARGIN, 104 }, "Готово", 1, ACT_CLOSE, 0, 36);
    pf_update(0, s->height - r->panel_h, s->width, r->panel_h, PF_UPDATE_PARTIAL);
}

/* «Вид»: масштаб, расположение (книжная, альбомная, разворот), контраст. */
#define VIEW_SHEET_H 760
static void draw_view_sheet(reader *r)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    int y = sheet_start(r, s, VIEW_SHEET_H, "Вид");
    static const char *const zoom[] = { "По странице", "По ширине", "Без полей" };
    static const int zoom_v[] = { VIEW_FIT_PAGE, VIEW_FIT_WIDTH, VIEW_CROP };
    ui_caption(r->text, s, UI_MARGIN, y + 34, "Масштаб");
    segments(r, s, y + 52, zoom, zoom_v, 3, r->opt.view, ACT_VIEW);
    y += 52 + SHEET_SEG_H + 24;
    static const char *const layout[] = { "Книжная", "Альбомная", "Разворот" };
    static const int layout_v[] = { 0, 1, 2 };
    ui_caption(r->text, s, UI_MARGIN, y + 34, "Расположение");
    segments(r, s, y + 52, layout, layout_v, 3, r->opt.spread ? 2 : r->opt.orient ? 1 : 0, ACT_LAYOUT);
    y += 52 + SHEET_SEG_H + 24;
    static const char *const contrast[] = { "Обычный", "Выше", "Высокий", "Максимум" };
    static const int contrast_v[] = { 0, 1, 2, 3 };
    ui_caption(r->text, s, UI_MARGIN, y + 34, "Контраст — для бледных сканов");
    segments(r, s, y + 52, contrast, contrast_v, 4, r->opt.contrast, ACT_CONTRAST);
    y += 52 + SHEET_SEG_H + 36;
    sheet_done(r, s, y);
}

/* «Подсветка»: яркость и теплота ползунками (тянуть или коснуться), выключить. */
#define LIGHT_SHEET_H 560
#define LIGHT_ROW_H 150
static void light_row(reader *r, pf_screen *s, int y, const char *label, int value, action act)
{
    ui_slider_row(r->text, s, y, LIGHT_ROW_H, label, value);
    add_control(r, (box){ 0, y + 60, s->width, 90 }, act, 0);
}

static void draw_light_sheet(reader *r)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    pf_light_get(&r->light_b, &r->light_w);
    int h = r->light_w >= 0 ? LIGHT_SHEET_H : LIGHT_SHEET_H - LIGHT_ROW_H;
    int y = sheet_start(r, s, h, "Подсветка");
    light_row(r, s, y, "Яркость", r->light_b, ACT_LIGHT_B);
    y += LIGHT_ROW_H;
    if (r->light_w >= 0) {
        light_row(r, s, y, "Теплота", r->light_w, ACT_LIGHT_W);
        y += LIGHT_ROW_H;
    }
    int gap = 16, bw = (s->width - 2 * UI_MARGIN - gap) / 2;
    button(r, s, (box){ UI_MARGIN, y + 20, bw, 104 }, r->light_b > 0 ? "Выключить" : "Включить", 0, ACT_LIGHT_OFF, 0,
           36);
    button(r, s, (box){ UI_MARGIN + bw + gap, y + 20, bw, 104 }, "Готово", 1, ACT_CLOSE, 0, 36);
    pf_update(0, s->height - r->panel_h, s->width, r->panel_h, PF_UPDATE_PARTIAL);
}

static void draw_keypad(reader *r)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    r->nctl = 0;
    int W = s->width, landscape = W > s->height;
    int bh = landscape ? 104 : 150, field_h = landscape ? 104 : 130;
    r->panel_h = 80 + 40 + field_h + 30 + 4 * (bh + 16) + 10 + UI_ROW + 30;
    gfx_fill(s, 0, 0, W, r->panel_h, UI_WHITE);
    gfx_fill(s, 0, r->panel_h - 2, W, 2, UI_TEXT3);
    int y = 80;
    char line[128];
    snprintf(line, sizeof line, "Перейти к странице (1–%d)", r->page_count);
    gfx_style ts = { FONT_MEDIUM, 44, UI_BLACK };
    gfx_text(r->text, s, UI_MARGIN, y, ts, line);
    y += 40;
    box field = { UI_MARGIN, y, W - 2 * UI_MARGIN, field_h };
    button(r, s, field, *r->keypad ? r->keypad : " ", 0, ACT_CANCEL, -1, landscape ? 56 : 70);
    r->nctl--; /* поле — не кнопка */
    y += field_h + 30;

    int gap = 16, bw = (W - 2 * UI_MARGIN - 2 * gap) / 3;
    static const char *const keys[] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "Стереть", "0", "Перейти" };
    for (int i = 0; i < 12; i++) {
        box b = { UI_MARGIN + (i % 3) * (bw + gap), y + (i / 3) * (bh + gap), bw, bh };
        action a = i == 9 ? ACT_BACKSPACE : i == 11 ? ACT_OK : ACT_DIGIT;
        int v = i == 10 ? 0 : i + 1;
        button(r, s, b, keys[i], i == 11, a, v, i == 9 || i == 11 ? UI_TEXT : 60);
    }
    y += 4 * (bh + gap) + 10;
    button(r, s, (box){ UI_MARGIN, y, W - 2 * UI_MARGIN, UI_ROW }, "Отмена", 0, ACT_CANCEL, 0, UI_TEXT);
    pf_update(0, 0, W, r->panel_h, PF_UPDATE_PARTIAL);
}

/* Содержание и закладки на весь экран, вкладками. В содержании — вложенность отступом,
 * справа страница, глава, в которой читаем, выделена; в закладках — страница и её глава. */
static void draw_toc(reader *r, int open)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    gfx *g = r->text;
    int W = s->width, H = s->height, M = UI_MARGIN;
    r->nctl = 0;
    gfx_fill(s, 0, 0, W, H, UI_WHITE);
    ui_status(g, s);
    int top = ui_content_top() + 70;
    int n = r->toc_tab ? r->bm.count : r->ntoc;
    r->toc_rows = (H - top - 30) / TOC_ROW_H;
    if (r->toc_rows < 1)
        r->toc_rows = 1;
    r->toc_pages = n ? (n + r->toc_rows - 1) / r->toc_rows : 1;
    int cur = toc_current(r, r->target);
    if (open)
        r->toc_page = !r->toc_tab && cur >= 0 ? cur / r->toc_rows : 0;
    if (r->toc_page >= r->toc_pages)
        r->toc_page = r->toc_pages - 1;
    ui_header_hits hits = ui_header(g, s, r->toc_tab ? "Закладки" : "Содержание", 1, r->toc_page, r->toc_pages);
    if (hits.back.w)
        add_control(r, (box){ hits.back.x, hits.back.y, hits.back.w, hits.back.h }, ACT_CLOSE, 0);
    if (hits.prev.w)
        add_control(r, (box){ hits.prev.x, hits.prev.y, hits.prev.w, hits.prev.h }, ACT_TOC_PREV, 0);
    if (hits.next.w)
        add_control(r, (box){ hits.next.x, hits.next.y, hits.next.w, hits.next.h }, ACT_TOC_NEXT, 0);

    /* Вкладки «Содержание · Закладки» (как в Литрес: выбранная — жирным и с чертой). */
    char bm_label[48];
    snprintf(bm_label, sizeof bm_label, "Закладки · %d", r->bm.count);
    const char *tabs[2] = { "Содержание", bm_label };
    int tx = M;
    for (int i = 0; i < 2; i++) {
        gfx_style st = { i == r->toc_tab ? FONT_BOLD : FONT_REGULAR, 36, i == r->toc_tab ? UI_BLACK : UI_TEXT3 };
        int w = gfx_text_width(g, st, tabs[i]);
        gfx_text(g, s, tx, top - 30, st, tabs[i]);
        if (i == r->toc_tab)
            gfx_rrect(g, s, tx, top - 12, w, 6, 3, UI_BLACK);
        add_control(r, (box){ tx - 20, top - 80, w + 40, 90 }, ACT_TOC_TAB, i);
        tx += w + 56;
    }
    gfx_fill(s, 0, top - 4, W, 2, UI_FAINT);

    gfx_style ps = { FONT_REGULAR, 34, UI_TEXT2 };
    int first = r->toc_page * r->toc_rows;
    if (n == 0) {
        gfx_style st = { FONT_REGULAR, 38, UI_TEXT2 };
        gfx_text_wrap(g, s, M, top + 80, W - 2 * M, 52, 3, st,
                      r->toc_tab ? "Закладок нет. Коснитесь правого верхнего угла страницы, чтобы поставить."
                                 : "В этой книге нет оглавления.");
    }
    for (int i = 0; i < r->toc_rows && first + i < n; i++) {
        int y = top + i * TOC_ROW_H;
        if (r->toc_tab) {
            int page = r->bm.pages[first + i];
            char num[32];
            snprintf(num, sizeof num, "Стр. %d", page + 1);
            int c = toc_current(r, page);
            gfx_style ns = { FONT_MEDIUM, 38, UI_BLACK };
            int nw = gfx_text_width(g, ns, num);
            gfx_icon_ex(g, s, ICON_BOOKMARK, M, y + TOC_ROW_H / 2 - 26, 52, 0, 1, UI_BLACK);
            gfx_text(g, s, M + 76, y + TOC_ROW_H / 2 + 13, ns, num);
            gfx_text_fit(g, s, M + 76 + nw + 32, y + TOC_ROW_H / 2 + 12, W - 2 * M - 76 - nw - 32, ps,
                         c >= 0 ? r->toc[c].title : "");
            add_control(r, (box){ 0, y, W, TOC_ROW_H }, ACT_BM_ITEM, page);
        } else {
            const doc_outline_item *it = &r->toc[first + i];
            int level = it->level > 4 ? 4 : it->level, x = M + level * 44, on = first + i == cur;
            if (on)
                gfx_rrect(g, s, M - 34, y + 30, 10, TOC_ROW_H - 60, 5, UI_BLACK);
            char num[16] = "";
            if (it->page >= 0)
                snprintf(num, sizeof num, "%d", it->page + 1);
            int nw = gfx_text_width(g, ps, num);
            gfx_text(g, s, W - M - nw, y + TOC_ROW_H / 2 + 12, ps, num);
            gfx_style st = { on ? FONT_MEDIUM : FONT_REGULAR, level == 0 ? 39.0f : 36.0f,
                             it->page >= 0 ? UI_BLACK : UI_TEXT3 };
            gfx_text_fit(g, s, x, y + TOC_ROW_H / 2 + 13, W - M - nw - 40 - x, st, it->title);
            if (it->page >= 0)
                add_control(r, (box){ 0, y, W, TOC_ROW_H }, ACT_TOC_ITEM, first + i);
        }
        if (i + 1 < r->toc_rows && first + i + 1 < n)
            gfx_fill(s, M, y + TOC_ROW_H - 2, W - 2 * M, 2, UI_FAINT);
    }
    pf_update(0, 0, W, H, open ? PF_UPDATE_FULL_HQ : PF_UPDATE_PARTIAL);
}

/* Подсказка при первом открытии книги: зоны касания и жесты поверх страницы. */
static void draw_hint(reader *r)
{
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    gfx *g = r->text;
    int W = s->width, H = s->height, top = H / TOP_ZONE_DIV, third = W / 3;
    r->nctl = 0;
    gfx_fill(s, 0, 0, W, top, 0xdd);
    gfx_fill(s, 0, top, third, H - top, UI_FAINT);
    gfx_fill(s, third, top, W - third, H - top, UI_WHITE);
    gfx_fill(s, 0, top, W, 3, UI_TEXT3);
    gfx_fill(s, third, top, 3, H - top, UI_TEXT3);
    gfx_fill(s, W - CORNER, 0, 3, top, UI_TEXT3);

    gfx_style zs = { FONT_MEDIUM, 46, UI_BLACK }, ds = { FONT_REGULAR, 34, UI_TEXT2 }, cs = { FONT_MEDIUM, 30, UI_BLACK };
    const char *menu = "Меню";
    gfx_text(g, s, (W - CORNER - gfx_text_width(g, zs, menu)) / 2, top / 2 + 16, zs, menu);
    gfx_icon_ex(g, s, ICON_BOOKMARK, W - CORNER / 2 - 30, top / 2 - 52, 60, 0, 1, UI_BLACK);
    gfx_text(g, s, W - CORNER / 2 - gfx_text_width(g, cs, "Закладка") / 2, top / 2 + 44, cs, "Закладка");
    gfx_icon(g, s, ICON_BACK, third / 2 - 40, H / 2 - 120, 80, 0, UI_BLACK);
    gfx_text(g, s, third / 2 - gfx_text_width(g, zs, "Назад") / 2, H / 2 + 20, zs, "Назад");
    int cx = third + (W - third) / 2;
    gfx_icon(g, s, ICON_NEXT, cx - 40, H / 2 - 120, 80, 0, UI_BLACK);
    gfx_text(g, s, cx - gfx_text_width(g, zs, "Вперёд") / 2, H / 2 + 20, zs, "Вперёд");
    gfx_icon(g, s, ICON_SUN, 30, top + 60, 56, 0, UI_BLACK);
    gfx_style ls = { FONT_REGULAR, 28, UI_TEXT2 };
    gfx_text_wrap(g, s, 30, top + 160, third - 60, 36, 3, ls, "Вверх-вниз у края — яркость");
    gfx_icon(g, s, ICON_SUN, W - 30 - 56, top + 60, 56, 0, UI_TEXT2);
    gfx_style rs = { FONT_REGULAR, 28, UI_TEXT2 };
    const char *warm[] = { "Вверх-вниз", "у края — теплота" };
    for (int i = 0; i < 2; i++)
        gfx_text(g, s, W - 30 - gfx_text_width(g, rs, warm[i]), top + 160 + i * 36, rs, warm[i]);

    static const char *const lines[] = {
        "Свайп влево или вправо — листать",
        "Долгое касание или кнопка меню — меню",
        "Удержание кнопки листания — через 10 стр.",
        "Коснитесь, чтобы начать",
    };
    int y = H - 330, x = third + 50;
    for (int i = 0; i < 4; i++) {
        gfx_style st = i == 3 ? (gfx_style){ FONT_MEDIUM, 36, UI_BLACK } : ds;
        gfx_text_fit(g, s, x, y + i * 64 + (i == 3 ? 24 : 0), W - x - UI_MARGIN, st, lines[i]);
    }
    pf_update(0, 0, W, H, PF_UPDATE_FULL_HQ);
}

/* Убрать всё поверх страницы: страница снова на весь экран. */
static void close_overlay(reader *r, double t)
{
    int was_full = r->overlay == OVERLAY_TOC || r->overlay == OVERLAY_HINT;
    r->overlay = OVERLAY_NONE;
    r->nctl = 0;
    r->dragging = 0;
    if (cache_has(r->cache, r->target) && !r->waiting) {
        r->need_full = was_full;
        show_target(r, t, 1);
    }
    arm_timer(r);
}

static void margins_progress_cb(void *user, int done, int total)
{
    reader *r = user;
    pf_screen *s = pf_get_screen();
    if (!s || !text_of(r))
        return;
    char line[96];
    snprintf(line, sizeof line, "Ищу поля страниц: %d из %d", done, total);
    int y = 300, h = 160, x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    gfx_fill(s, x, y, w, h, UI_WHITE);
    gfx_rrect_line(r->text, s, x, y, w, h, 22, 3, UI_BLACK);
    gfx_style st = { FONT_REGULAR, 38, UI_BLACK };
    gfx_text(r->text, s, x + 40, y + 95, st, line);
    pf_update(x, y, w, h, PF_UPDATE_PARTIAL);
}

/* Новый способ рисовать страницы (масштаб, контраст): кэш — заново, та же страница. */
static void redraw_with_view(reader *r, double t)
{
    r->requests |= READER_SAVE_BOOK;
    view_settings v = current_view(r);
    renderer_set_view(r->render, &v);
    cache_clear(r->cache);
    r->overlay = OVERLAY_NONE;
    r->nctl = 0;
    r->need_full = 1;
    r->shown = -1;
    r->links_for = -1;
    go_to(r, r->target, 0, t);
}

static void set_view(reader *r, int mode, double t)
{
    if (mode == VIEW_CROP && !r->opt.has_crop) {
        /* Поля книги ищутся один раз и запоминаются в positions.txt. */
        renderer_request(r->render, NULL, 0, 0);
        margins_progress_cb(r, 0, 1);
        if (margins_find(r->opt.path, &r->opt.doc, r->opt.crop, margins_progress_cb, r) == 0) {
            r->opt.has_crop = 1;
            r->requests |= READER_SAVE_BOOK;
        } else {
            log_msg("поля не найдены — масштаб прежний");
            close_overlay(r, t);
            return;
        }
    }
    if (mode == r->opt.view) {
        close_overlay(r, t);
        return;
    }
    log_msg("масштаб: %d → %d", r->opt.view, mode);
    r->opt.view = mode;
    redraw_with_view(r, t);
}

/* Расположение: книжная, альбомная, разворот. Ориентацию меняет программа — книга
 * открывается заново на той же странице. */
static void set_layout(reader *r, int layout, double t)
{
    int orient = layout == 0 ? 0 : r->opt.orient ? r->opt.orient : 1, spread = layout == 2;
    if (orient == r->opt.orient && spread == r->opt.spread) {
        close_overlay(r, t);
        return;
    }
    log_msg("расположение: ориентация %d → %d, разворот %d → %d", r->opt.orient, orient, r->opt.spread, spread);
    r->opt.orient = orient;
    r->opt.spread = spread;
    r->overlay = OVERLAY_NONE;
    r->requests |= READER_SAVE_BOOK | READER_WANT_REOPEN;
}

static void set_contrast(reader *r, int level, double t)
{
    if (level == r->opt.contrast) {
        close_overlay(r, t);
        return;
    }
    log_msg("контраст: %d → %d", r->opt.contrast, level);
    r->opt.contrast = level;
    redraw_with_view(r, t);
}

/* Перейти к странице из панели, содержания, закладок, ссылки или клавиатуры: запоминаем,
 * откуда, — на странице появится чип «← стр. N». */
static void jump(reader *r, int page, double t)
{
    log_msg("переход к стр. %d", page + 1);
    if (align(r, page) != r->target) {
        r->back_page = r->target;
        r->chip = 1;
    }
    r->overlay = OVERLAY_NONE;
    r->nctl = 0;
    r->need_full = 1;
    go_to(r, page, 0, t);
}

/* Закладка на показанной странице: поставить или снять; на странице — лента в углу. */
static void toggle_bookmark(reader *r)
{
    pf_screen *s = pf_get_screen();
    page_image *img = cache_get(r->cache, r->shown);
    int on = bookmarks_toggle(&r->bm, r->target);
    log_msg("закладка на стр. %d: %s", r->target + 1, on ? "поставлена" : "снята");
    if (!s || !img || r->overlay != OVERLAY_NONE || !text_of(r))
        return;
    /* Угол: страница заново, сверху — лента, если она есть. */
    int x = s->width - CORNER, w = CORNER, h = CORNER;
    for (int y = 0; y < h && y + r->offset < img->height; y++)
        memcpy(s->pixels + (size_t)y * s->stride + x, img->pixels + (size_t)(y + r->offset) * img->stride + x,
               (size_t)w);
    if (page_bookmarked(r, r->target))
        draw_ribbon(r, s);
    pf_update(x, 0, w, h, PF_UPDATE_PARTIAL);
}

/* Ссылки показанной картинки в координатах экрана (загружаются при первом касании). */
static void load_links(reader *r)
{
    if (r->links_for == r->shown && r->links_offset == r->offset)
        return;
    free(r->links);
    r->links = NULL;
    r->nlinks = 0;
    r->links_for = r->shown;
    r->links_offset = r->offset;
    page_image *img = cache_get(r->cache, r->shown);
    doc *d = udoc(r);
    if (!img || !d)
        return;
    for (int k = 0; k < img->npages; k++) {
        doc_link *l = NULL;
        int n = doc_links(d, img->lpage[k], &l);
        if (n > 0) {
            doc_link *all = realloc(r->links, sizeof *all * (size_t)(r->nlinks + n));
            if (all) {
                r->links = all;
                for (int i = 0; i < n; i++) {
                    doc_link *o = &r->links[r->nlinks++];
                    float sc = img->lscale[k];
                    o->rect.x0 = img->lx[k] + (l[i].rect.x0 - img->lx0[k]) * sc;
                    o->rect.x1 = img->lx[k] + (l[i].rect.x1 - img->lx0[k]) * sc;
                    o->rect.y0 = (l[i].rect.y0 - img->ly0[k]) * sc - r->offset;
                    o->rect.y1 = (l[i].rect.y1 - img->ly0[k]) * sc - r->offset;
                    o->page = l[i].page;
                }
            }
        }
        free(l);
    }
}

/* Ссылка под касанием (с запасом в 12 точек — палец толще ссылки); -1 — нет. */
static int link_at(reader *r, int x, int y)
{
    load_links(r);
    const int pad = 12;
    for (int i = 0; i < r->nlinks; i++) {
        const doc_rect *b = &r->links[i].rect;
        if (x >= b->x0 - pad && x <= b->x1 + pad && y >= b->y0 - pad && y <= b->y1 + pad)
            return r->links[i].page;
    }
    return -1;
}

static void light_apply(reader *r, int act, int x)
{
    pf_screen *s = pf_get_screen();
    if (!s)
        return;
    int v = ui_slider_percent(s, x);
    if (act == ACT_LIGHT_B)
        r->light_b = v;
    else
        r->light_w = v;
    pf_light_set(r->light_b, r->light_w);
}

static void open_panel(reader *r)
{
    log_msg("панель");
    if (pf_light_get(&r->light_b, &r->light_w) != 0)
        r->light_b = r->light_w = -1;
    r->overlay = OVERLAY_PANEL;
    draw_panel(r);
}

static void on_control(reader *r, const control *c, int x, double t)
{
    switch (c->act) {
    case ACT_CLOSE:
        close_overlay(r, t);
        break;
    case ACT_LIBRARY:
        r->overlay = OVERLAY_NONE;
        r->requests |= READER_WANT_LIBRARY;
        break;
    case ACT_GOTO:
        r->overlay = OVERLAY_KEYPAD;
        r->keypad[0] = 0;
        if (cache_has(r->cache, r->target))
            blit(cache_get(r->cache, r->target), r->offset, pf_get_screen()); /* без панели под клавиатурой */
        draw_keypad(r);
        break;
    case ACT_VIEW:
        set_view(r, c->value, t);
        break;
    case ACT_LAYOUT:
        set_layout(r, c->value, t);
        break;
    case ACT_CONTRAST:
        set_contrast(r, c->value, t);
        break;
    case ACT_SLIDER: {
        pf_screen *s = pf_get_screen();
        if (s)
            jump(r, slider_page(r, s, x), t);
        break;
    }
    case ACT_TOC:
        load_toc(r);
        r->overlay = OVERLAY_TOC;
        r->toc_tab = 0;
        draw_toc(r, 1);
        break;
    case ACT_TOC_TAB:
        r->toc_tab = c->value;
        r->toc_page = 0;
        draw_toc(r, 0);
        break;
    case ACT_TOC_ITEM:
        if (c->value >= 0 && c->value < r->ntoc && r->toc[c->value].page >= 0)
            jump(r, r->toc[c->value].page, t);
        break;
    case ACT_BM_ITEM:
        jump(r, c->value, t);
        break;
    case ACT_TOC_PREV:
    case ACT_TOC_NEXT: {
        int p = r->toc_page + (c->act == ACT_TOC_NEXT ? 1 : -1);
        if (p >= 0 && p < r->toc_pages) {
            r->toc_page = p;
            draw_toc(r, 0);
        }
        break;
    }
    case ACT_BOOKMARK:
        bookmarks_toggle(&r->bm, r->target);
        draw_panel(r); /* значок закладки в панели — залитый или нет */
        break;
    case ACT_VIEW_SHEET:
        if (cache_has(r->cache, r->target))
            blit(cache_get(r->cache, r->target), r->offset, pf_get_screen());
        r->overlay = OVERLAY_VIEW;
        draw_view_sheet(r);
        {
            pf_screen *s = pf_get_screen();
            if (s) /* убрать верх панели: страница там снова видна */
                pf_update(0, 0, s->width, PANEL_TOP_H, PF_UPDATE_PARTIAL);
        }
        break;
    case ACT_LIGHT_SHEET:
        if (cache_has(r->cache, r->target))
            blit(cache_get(r->cache, r->target), r->offset, pf_get_screen());
        r->overlay = OVERLAY_LIGHT;
        draw_light_sheet(r);
        {
            pf_screen *s = pf_get_screen();
            if (s)
                pf_update(0, 0, s->width, PANEL_TOP_H, PF_UPDATE_PARTIAL);
        }
        break;
    case ACT_LIGHT_B:
    case ACT_LIGHT_W:
        light_apply(r, c->act, x);
        draw_light_sheet(r);
        break;
    case ACT_LIGHT_OFF:
        if (r->light_b > 0) {
            r->light_b = 0;
        } else {
            r->light_b = 30;
        }
        pf_light_set(r->light_b, r->light_w);
        draw_light_sheet(r);
        break;
    case ACT_BACK_JUMP: {
        int to = r->back_page;
        if (to >= 0) {
            log_msg("возврат на стр. %d", to + 1);
            r->back_page = -1; /* вернулись — чип и кнопка в панели исчезают */
            r->chip = 0;
            r->overlay = OVERLAY_NONE;
            r->nctl = 0;
            r->need_full = 1;
            go_to(r, to, 0, t);
        }
        break;
    }
    case ACT_DIGIT: {
        size_t n = strlen(r->keypad);
        if (n < 6 && !(n == 0 && c->value == 0)) {
            r->keypad[n] = (char)('0' + c->value);
            r->keypad[n + 1] = 0;
        }
        draw_keypad(r);
        break;
    }
    case ACT_BACKSPACE: {
        size_t n = strlen(r->keypad);
        if (n > 0)
            r->keypad[n - 1] = 0;
        draw_keypad(r);
        break;
    }
    case ACT_OK: {
        int p = atoi(r->keypad);
        if (p < 1 || p > r->page_count) {
            r->keypad[0] = 0;
            draw_keypad(r);
            break;
        }
        jump(r, p - 1, t);
        break;
    }
    case ACT_CANCEL:
        close_overlay(r, t);
        break;
    }
}

static void overlay_tap(reader *r, int x, int y, double t)
{
    if (r->overlay == OVERLAY_HINT) {
        close_overlay(r, t);
        return;
    }
    for (int i = 0; i < r->nctl; i++) {
        const box *b = &r->ctl[i].b;
        if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) {
            control c = r->ctl[i];
            on_control(r, &c, x, t);
            return;
        }
    }
    pf_screen *s = pf_get_screen();
    int h = s ? s->height : 1872;
    /* Мимо кнопок, на видной странице — закрыть. */
    if ((r->overlay == OVERLAY_PANEL && y > PANEL_TOP_H && y < h - PANEL_BOTTOM_H) ||
        ((r->overlay == OVERLAY_VIEW || r->overlay == OVERLAY_LIGHT) && y < h - r->panel_h) ||
        (r->overlay == OVERLAY_KEYPAD && y >= r->panel_h))
        close_overlay(r, t);
}

/* Жесты на странице. */
static void on_gesture(reader *r, const gesture *g, double t)
{
    pf_screen *s = pf_get_screen();
    int w = s ? s->width : 1, h = s ? s->height : 1;
    if (r->overlay != OVERLAY_NONE) {
        if (g->kind == GESTURE_TAP) {
            overlay_tap(r, g->x, g->y, t);
        } else if (r->overlay == OVERLAY_TOC && (g->kind == GESTURE_SWIPE_LEFT || g->kind == GESTURE_SWIPE_RIGHT)) {
            control c = { { 0, 0, 0, 0 }, g->kind == GESTURE_SWIPE_LEFT ? ACT_TOC_NEXT : ACT_TOC_PREV, 0 };
            on_control(r, &c, 0, t);
        } else if (r->overlay == OVERLAY_HINT) {
            close_overlay(r, t);
        }
        return;
    }
    log_msg("%s (%d, %d)", gesture_name(g->kind), g->x, g->y);
    switch (g->kind) {
    case GESTURE_TAP: {
        if (r->chip_box.w && g->x >= r->chip_box.x && g->x < r->chip_box.x + r->chip_box.w &&
            g->y >= r->chip_box.y && g->y < r->chip_box.y + r->chip_box.h) {
            control c = { { 0, 0, 0, 0 }, ACT_BACK_JUMP, 0 };
            on_control(r, &c, 0, t);
            break;
        }
        if (g->y < CORNER && g->x >= w - CORNER) {
            toggle_bookmark(r);
            break;
        }
        int link = g->y >= h / TOP_ZONE_DIV ? link_at(r, g->x, g->y) : -1;
        if (link >= 0) {
            log_msg("ссылка → стр. %d", link + 1);
            jump(r, link, t);
        } else if (g->y < h / TOP_ZONE_DIV) {
            open_panel(r);
        } else if (g->x < w / 3) {
            step(r, -1, t);
        } else {
            step(r, 1, t);
        }
        break;
    }
    case GESTURE_LONG:
        open_panel(r);
        break;
    case GESTURE_SWIPE_LEFT:
        step(r, 1, t);
        break;
    case GESTURE_SWIPE_RIGHT:
        step(r, -1, t);
        break;
    case GESTURE_SWIPE_UP:
    case GESTURE_SWIPE_DOWN:
        if (g->kind == GESTURE_SWIPE_DOWN && g->y < h / TOP_ZONE_DIV)
            open_panel(r); /* вниз от верхнего края — панель, как шторка */
        else
            step(r, g->kind == GESTURE_SWIPE_UP ? 1 : -1, t);
        break;
    default:
        break;
    }
}

static void on_key(reader *r, const pf_event *ev)
{
    double t = ev->time_ms;
    log_msg("кнопка %d%s, в очереди ввода %d", ev->raw_key, ev->type == PF_EVENT_KEY_REPEAT ? " (удержание)" : "",
            pf_pending_input());
    if (ev->type == PF_EVENT_KEY_REPEAT) {
        /* Удержание ‹ › — через JUMP_PAGES страниц, пока держат. */
        if (r->overlay == OVERLAY_NONE && (ev->key == PF_KEY_NEXT || ev->key == PF_KEY_PREV)) {
            int p = r->target + (ev->key == PF_KEY_NEXT ? JUMP_PAGES : -JUMP_PAGES);
            p = p < 0 ? 0 : p >= r->page_count ? r->page_count - 1 : p;
            if (align(r, p) != r->target)
                go_to(r, p, 0, t);
        }
        return;
    }
    switch (r->overlay) {
    case OVERLAY_NONE:
        if (ev->key == PF_KEY_NEXT)
            step(r, 1, t);
        else if (ev->key == PF_KEY_PREV)
            step(r, -1, t);
        else if (ev->key == PF_KEY_MENU)
            open_panel(r);
        break;
    case OVERLAY_TOC:
        if (ev->key == PF_KEY_NEXT || ev->key == PF_KEY_PREV) {
            control c = { { 0, 0, 0, 0 }, ev->key == PF_KEY_NEXT ? ACT_TOC_NEXT : ACT_TOC_PREV, 0 };
            on_control(r, &c, 0, t);
        } else {
            close_overlay(r, t);
        }
        break;
    case OVERLAY_PANEL:
        /* С панелью кнопки листания закрывают её и листают. */
        close_overlay(r, t);
        if (ev->key == PF_KEY_NEXT)
            step(r, 1, t);
        else if (ev->key == PF_KEY_PREV)
            step(r, -1, t);
        break;
    default:
        close_overlay(r, t);
        break;
    }
}

/* Перетаскивание ползунков: страница в панели, яркость и теплота в «Подсветке». Нажали на
 * дорожку — ведём, значение под пальцем обновляется быстрым обновлением не чаще раза
 * в DRAG_REDRAW_MS, отпустили — переход (страница) или значение остаётся (подсветка).
 * 1 — событие забрано. */
#define DRAG_REDRAW_MS 150

static int slider_drag(reader *r, const pf_event *ev)
{
    pf_screen *s = pf_get_screen();
    if (!s)
        return 0;
    int y0 = s->height - PANEL_BOTTOM_H;
    if (ev->type == PF_EVENT_POINTER_DOWN) {
        if (r->overlay == OVERLAY_PANEL && ev->y >= y0 + 85 && ev->y < y0 + 195) {
            r->dragging = ACT_SLIDER;
            r->drag_page = slider_page(r, s, ev->x);
            draw_slider(r, s, r->drag_page);
            pf_update(0, y0, s->width, SLIDER_ROWS_H, PF_UPDATE_FAST_BW);
        } else if (r->overlay == OVERLAY_LIGHT) {
            for (int i = 0; i < r->nctl; i++) {
                const box *b = &r->ctl[i].b;
                if ((r->ctl[i].act == ACT_LIGHT_B || r->ctl[i].act == ACT_LIGHT_W) && ev->x >= b->x &&
                    ev->x < b->x + b->w && ev->y >= b->y && ev->y < b->y + b->h) {
                    r->dragging = r->ctl[i].act;
                    r->drag_box = (box){ 0, b->y - 60, s->width, 150 };
                    light_apply(r, r->dragging, ev->x);
                    break;
                }
            }
            if (!r->dragging)
                return 0;
        } else {
            return 0;
        }
        r->drag_drawn = clock_ms();
        return 1;
    }
    if (!r->dragging)
        return 0;
    double now = clock_ms();
    if (ev->type == PF_EVENT_POINTER_MOVE) {
        if (now - r->drag_drawn < DRAG_REDRAW_MS)
            return 1;
        if (r->dragging == ACT_SLIDER) {
            int p = slider_page(r, s, ev->x);
            if (p != r->drag_page) {
                r->drag_page = p;
                draw_slider(r, s, p);
                pf_update(0, y0, s->width, SLIDER_ROWS_H, PF_UPDATE_FAST_BW);
            }
        } else {
            light_apply(r, r->dragging, ev->x);
            box b = r->drag_box;
            light_row(r, s, b.y, r->dragging == ACT_LIGHT_B ? "Яркость" : "Теплота",
                      r->dragging == ACT_LIGHT_B ? r->light_b : r->light_w, (action)r->dragging);
            r->nctl--; /* light_row добавила строку повторно */
            pf_update(0, b.y, s->width, b.h, PF_UPDATE_FAST_BW);
        }
        r->drag_drawn = now;
    } else if (ev->type == PF_EVENT_POINTER_UP) {
        int what = r->dragging;
        r->dragging = 0;
        if (what == ACT_SLIDER) {
            jump(r, slider_page(r, s, ev->x), ev->time_ms);
        } else {
            light_apply(r, what, ev->x);
            draw_light_sheet(r);
        }
    }
    return 1;
}

/* ---- Подсветка жестом вдоль края ----
 * Палец у левого края ведут вверх-вниз — яркость, у правого — теплота. Значение меняется
 * сразу, пока ведут (не чаще раза в DRAG_REDRAW_MS), сверху — указатель уровня быстрым
 * обновлением; отпустили — указатель убирается. Касание у края и свайп поперёк — как обычно
 * (листание). Если прошивка не шлёт движения пальца — уровень меняется по отпусканию. */

/* Вернуть страницу в прямоугольник b (убрать нарисованное поверх). */
static void restore_page(reader *r, pf_screen *s, box b)
{
    page_image *img = r->shown >= 0 ? cache_get(r->cache, r->shown) : NULL;
    for (int y = b.y; y < b.y + b.h && y < s->height; y++) {
        if (y < 0)
            continue;
        uint8_t *dst = s->pixels + (size_t)y * s->stride + b.x;
        int sy = y + r->offset, w = 0;
        if (img && sy < img->height && b.x < img->width) {
            w = b.x + b.w <= img->width ? b.w : img->width - b.x;
            memcpy(dst, img->pixels + (size_t)sy * img->stride + b.x, (size_t)w);
        }
        if (w < b.w)
            memset(dst + w, 255, (size_t)(b.w - w));
    }
}

/* Указатель уровня: чёрная плашка сверху по центру — значок, название, полоса, проценты. */
static void draw_edge_meter(reader *r, pf_screen *s, int v)
{
    if (!text_of(r))
        return;
    gfx *g = r->text;
    int w = 640, h = 132, x = (s->width - w) / 2, y = 64;
    r->edge_box = (box){ x - 8, y - 8, w + 16, h + 16 };
    gfx_rrect(g, s, x - 8, y - 8, w + 16, h + 16, (h + 16) / 2.0f, UI_WHITE);
    gfx_rrect(g, s, x, y, w, h, h / 2.0f, UI_BLACK);
    gfx_icon(g, s, ICON_SUN, x + 44, y + (h - 60) / 2, 60, 0, UI_WHITE);
    char label[48];
    snprintf(label, sizeof label, "%s  %d %%", r->edge == 1 ? "Яркость" : "Теплота", v);
    gfx_style st = { FONT_MEDIUM, 36, UI_WHITE };
    int tx = x + 136, tw = w - 136 - 60;
    gfx_text(g, s, tx, y + 58, st, label);
    gfx_rrect_line(g, s, tx, y + 82, tw, 16, 8, 2.5f, UI_WHITE);
    int fw = tw * v / 100;
    if (fw > 16)
        gfx_rrect(g, s, tx, y + 82, fw, 16, 8, UI_WHITE);
}

static void edge_set(reader *r, pf_screen *s, int y, int final)
{
    double now = clock_ms();
    if (!final && now - r->edge_drawn < DRAG_REDRAW_MS)
        return;
    int v = r->edge_v0 + (r->edge_y0 - y) * 100 / (s->height / EDGE_RANGE_DIV);
    v = v < 0 ? 0 : v > 100 ? 100 : v;
    if (v != r->edge_v || final) {
        if (r->edge == 1)
            r->light_b = v;
        else
            r->light_w = v;
        double t0 = clock_ms();
        pf_light_set(r->light_b, r->light_w);
        r->edge_set_ms += clock_ms() - t0;
        r->edge_sets++;
        r->edge_v = v;
    }
    if (!final) {
        draw_edge_meter(r, s, v);
        pf_update(r->edge_box.x, r->edge_box.y, r->edge_box.w, r->edge_box.h, PF_UPDATE_FAST_BW);
    } else if (r->edge_box.w) {
        restore_page(r, s, r->edge_box);
        pf_update(r->edge_box.x, r->edge_box.y, r->edge_box.w, r->edge_box.h, PF_UPDATE_PARTIAL);
        r->edge_box = (box){ 0, 0, 0, 0 };
    }
    r->edge_drawn = clock_ms();
}

/* Жест начат: узнать уровень у прошивки (не при каждом касании у края — оно листает). */
static int edge_begin(reader *r)
{
    int b, w;
    if (pf_light_get(&b, &w) != 0 || (r->edge == 2 && w < 0)) {
        r->edge = 0; /* нет подсветки или тёплого света */
        return 0;
    }
    r->light_b = b;
    r->light_w = w;
    r->edge_v0 = r->edge_v = r->edge == 1 ? b : w;
    r->edge_sets = 0;
    r->edge_set_ms = 0;
    r->edge_drawn = 0;
    r->edge_box = (box){ 0, 0, 0, 0 };
    r->edge_active = 1;
    r->gs.down = 0; /* отпускание — конец подсветки, а не свайп */
    log_msg("подсветка жестом: %s, было %d %%", r->edge == 1 ? "яркость" : "теплота", r->edge_v0);
    return 1;
}

/* 1 — событие забрано жестом подсветки. */
static int edge_drag(reader *r, const pf_event *ev)
{
    pf_screen *s = pf_get_screen();
    if (!s || r->overlay != OVERLAY_NONE) {
        r->edge = r->edge_active = 0;
        return 0;
    }
    switch (ev->type) {
    case PF_EVENT_POINTER_DOWN: {
        r->edge = r->edge_active = 0;
        /* Пятая часть ширины: на книге проводили и в 2–3 см от края. */
        int edge_w = s->width / EDGE_DIV;
        int side = ev->x < edge_w ? 1 : ev->x >= s->width - edge_w ? 2 : 0;
        if (!side || ev->y < s->height / TOP_ZONE_DIV)
            return 0; /* сверху — панель и закладка */
        r->edge = side;
        r->edge_x0 = ev->x;
        r->edge_y0 = ev->y;
        return 0; /* пока это может быть и касание, и свайп */
    }
    case PF_EVENT_POINTER_MOVE: {
        if (!r->edge)
            return 0;
        int dx = abs(ev->x - r->edge_x0), dy = abs(ev->y - r->edge_y0);
        if (!r->edge_active) {
            if (dx > EDGE_START && dx > dy) {
                r->edge = 0; /* поперёк — это листание */
                return 0;
            }
            if (dy < EDGE_START || dy < 2 * dx || !edge_begin(r))
                return 0;
        }
        edge_set(r, s, ev->y, 0);
        return 1;
    }
    case PF_EVENT_POINTER_LONG:
        return r->edge_active; /* ведут вдоль края — долгое касание не открывает панель */
    case PF_EVENT_POINTER_UP: {
        if (!r->edge)
            return 0;
        if (!r->edge_active) {
            int dx = abs(ev->x - r->edge_x0), dy = abs(ev->y - r->edge_y0);
            /* Движения не приходили: уровень — по отпусканию. */
            if (dy < GESTURE_SWIPE_MIN || dy < 2 * dx || !edge_begin(r)) {
                r->edge = 0;
                return 0;
            }
        }
        edge_set(r, s, ev->y, 1);
        log_msg("подсветка жестом: %s %d → %d %%, применений %d, в среднем %.1f ms", r->edge == 1 ? "яркость" : "теплота",
                r->edge_v0, r->edge_v, r->edge_sets, r->edge_sets ? r->edge_set_ms / r->edge_sets : 0.0);
        r->edge = r->edge_active = 0;
        return 1;
    }
    default:
        return 0;
    }
}

void reader_on_event(const pf_event *ev, void *user)
{
    reader *r = user;
    if (!r->render) {
        /* Документ не открыт: экран с ошибкой, по любой кнопке или тапу — библиотека. */
        if (ev->type == PF_EVENT_START)
            open_document(r);
        if (ev->type == PF_EVENT_START && r->render) {
            int p = r->opt.start_page;
            go_to(r, p < 0 ? 0 : p >= r->page_count ? r->page_count - 1 : p, 0, ev->time_ms);
        } else if (ev->type == PF_EVENT_SHOW) {
            show_message(r, "Не удалось открыть файл", r->error);
        } else if (ev->type == PF_EVENT_KEY_DOWN || ev->type == PF_EVENT_POINTER_UP) {
            r->requests |= READER_WANT_LIBRARY;
        }
        return;
    }

    switch (ev->type) {
    case PF_EVENT_SHOW:
        if (r->overlay == OVERLAY_PANEL)
            draw_panel(r);
        else if (r->overlay == OVERLAY_VIEW)
            draw_view_sheet(r);
        else if (r->overlay == OVERLAY_LIGHT)
            draw_light_sheet(r);
        else if (r->overlay == OVERLAY_KEYPAD)
            draw_keypad(r);
        else if (r->overlay == OVERLAY_TOC)
            draw_toc(r, 0);
        else if (r->overlay == OVERLAY_HINT)
            draw_hint(r);
        else
            on_show(r, ev->time_ms);
        break;
    case PF_EVENT_TIMER:
        on_timer(r);
        break;
    case PF_EVENT_KEY_DOWN:
    case PF_EVENT_KEY_REPEAT:
        on_key(r, ev);
        break;
    case PF_EVENT_POINTER_DOWN:
    case PF_EVENT_POINTER_LONG:
    case PF_EVENT_POINTER_MOVE:
    case PF_EVENT_POINTER_UP: {
        if (slider_drag(r, ev) || edge_drag(r, ev))
            break;
        gesture g = gesture_feed(&r->gs, ev);
        if (g.kind != GESTURE_NONE)
            on_gesture(r, &g, ev->time_ms);
        break;
    }
    case PF_EVENT_BACKGROUND:
        log_msg("в фон");
        log_sync();
        break;
    case PF_EVENT_EXIT: {
        size_t peak;
        doc_mem_stats(NULL, &peak);
        log_msg("выход, стр. %d, пик кучи MuPDF %.1f MB", r->shown + 1, peak / 1048576.0);
        renderer_stop_async(r->render);
        log_sync();
        break;
    }
    default:
        break;
    }
}

const char *reader_path(const reader *r)
{
    return r->opt.path;
}

int reader_page(const reader *r)
{
    return r->target;
}

int reader_page_count(const reader *r)
{
    return r->render ? r->page_count : 0;
}

const reader_options *reader_current_options(const reader *r)
{
    return &r->opt;
}

int reader_take_requests(reader *r)
{
    int req = r->requests;
    r->requests = 0;
    return req;
}

void reader_pause(reader *r)
{
    r->paused = 1;
    r->waiting = 0;
    r->overlay = OVERLAY_NONE;
    r->nctl = 0;
    if (r->render)
        renderer_request(r->render, NULL, 0, 0);
}

void reader_resume(reader *r, double t)
{
    r->paused = 0;
    r->need_full = 1;
    if (!r->render) {
        show_message(r, "Не удалось открыть файл", r->error);
        return;
    }
    collect(r);
    if (cache_has(r->cache, r->target))
        show_target(r, t, 1);
    else
        go_to(r, r->target, r->target_offset, t);
}
