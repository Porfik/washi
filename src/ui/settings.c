#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../base/config.h"
#include "../base/dirpath.h"
#include "../base/log.h"
#include "folderpick.h"
#include "widgets.h"

/* Строки всех экранов. Первые R_ITEMS — параметры из config.txt (описаны в items[]),
 * остальное — переходы и действия. */
typedef enum {
    R_FONT,
    R_REFRESH,
    R_HINT,
    R_CACHE,
    R_AHEAD,
    R_BEHIND,
    R_BUSY,
    R_THREADS,
    R_WAITING,
    R_STORE,
    R_JPX,
    R_JPEG,
    R_ICC,
    R_ITEMS,
    R_SPEED = R_ITEMS, /* → экран «Скорость и память» */
    R_IMAGES,          /* → экран «Картинки в книгах» */
    R_SYSTEM,
    R_STATS,
    R_DOWNLOADS, /* → выбор папки для загрузок */
    R_QUIT,
    R_STOCK,
    R_COUNT
} row_id;

/* Экраны: главный, два вложенных списка, выбор значения и «Штатный главный экран». */
enum { SC_MAIN, SC_SPEED, SC_IMAGES, SC_CHOICE, SC_STOCK, SC_FOLDER };

/* Вариант на экране выбора: value — значение в config.txt, label — как называется (и что
 * видно справа в списке), text — что изменится. */
typedef struct {
    int value;
    const char *label, *text;
} choice;

#define MAX_CHOICES 8
#define CH(a) a, (int)(sizeof a / sizeof *a)

/* Параметр из config.txt: ключ, названия, варианты (или переключатель вкл/выкл). */
typedef struct {
    const char *key;
    const char *head;  /* заголовок экрана выбора */
    const char *title; /* название строки в списке */
    const char *sub;   /* подпись под названием */
    const char *intro; /* на экране выбора: что это такое */
    const char *unit;  /* как показать значение, которого нет среди вариантов (printf) */
    const choice *ch;
    int nch;
    int sw;  /* 1 — переключатель вкл/выкл, а не выбор из списка */
    int inv; /* переключатель: «вкл» — значение 0 в config.txt */
} item;

static const choice ch_font[] = {
    { 20, "20 пт", "Мелкий: на странице больше текста" },
    { 24, "24 пт", "Небольшой" },
    { 28, "28 пт", "Обычный — по умолчанию" },
    { 34, "34 пт", "Крупный" },
    { 42, "42 пт", "Очень крупный: страниц будет заметно больше" },
};
static const choice ch_refresh[] = {
    { 3, "Раз в 3 страницы", "Экран чище всего, листание чуть медленнее" },
    { 6, "Раз в 6 страниц", "По умолчанию: чисто и быстро" },
    { 10, "Раз в 10 страниц", "Реже мигает, следы прошлых страниц заметнее" },
    { 20, "Раз в 20 страниц", "Мигает редко, на светлых местах копится «шлейф»" },
    { 0, "Никогда", "Без мигания; следы страниц копятся, пока не обновите экран сами" },
};
static const choice ch_cache[] = {
    { 32, "32 МБ", "≈ 12 страниц: меньше памяти, возврат назад иногда рисуется заново" },
    { 64, "64 МБ", "≈ 25 страниц — по умолчанию" },
    { 96, "96 МБ", "≈ 38 страниц: листать туда-сюда быстрее" },
    { 128, "128 МБ", "≈ 50 страниц: быстрее всего, но занимает много памяти книги" },
};
static const choice ch_ahead[] = {
    { 0, "Не готовить", "Страница рисуется только по нажатию" },
    { 1, "1 страница", "Следующая страница готова заранее" },
    { 2, "2 страницы", "По умолчанию" },
    { 3, "3 страницы", "Для быстрого листания вперёд" },
    { 4, "4 страницы", "Больше всего; процессор дольше занят в фоне" },
};
static const choice ch_behind[] = {
    { 0, "Не готовить", "Возврат на страницу назад рисуется по нажатию" },
    { 1, "1 страница", "По умолчанию" },
    { 2, "2 страницы", "Листание назад почти без ожидания" },
    { 3, "3 страницы", "Больше всего; процессор дольше занят в фоне" },
};
static const choice ch_busy[] = {
    { 0, "Не пропускать", "Каждое нажатие показывает свою страницу, серия листается дольше" },
    { 300, "Немного (300 мс)", "Нажатия подряд чаще чем раз в 0,3 с не показываются по одному" },
    { 470, "Обычно (470 мс)", "По умолчанию: экран успевает обновиться" },
    { 700, "Сильнее (700 мс)", "Быстрая серия нажатий покажет только последнюю страницу" },
    { 1000, "Почти всё (1 с)", "Серия нажатий покажет сразу страницу, где остановились" },
};
static const choice ch_threads[] = {
    { 1, "1 поток", "Меньше нагрузка и память, страницы рисуются медленнее" },
    { 2, "2 потока", "По умолчанию: по потоку на каждое ядро" },
};
static const choice ch_store[] = {
    { 16, "16 МБ", "Меньше памяти; картинки и шрифты чаще разбираются заново" },
    { 32, "32 МБ", "Немного" },
    { 64, "64 МБ", "По умолчанию" },
    { 96, "96 МБ", "Для книг с большими картинками: листание быстрее, памяти больше" },
};
static const choice ch_jpx[] = {
    { 100, "100 % — точно", "Качество как у MuPDF, самая медленная распаковка" },
    { 85, "85 %", "Почти как есть, немного быстрее" },
    { 70, "70 % — быстро", "В 2–3 раза быстрее, фото чуть мягче" },
    { 50, "50 % — очень быстро", "Ещё быстрее, фото заметно мягче" },
};

/* Порядок — как в row_id. */
static const item items[R_ITEMS] = {
    [R_FONT] = { "reflow_em", "Размер шрифта", "Размер шрифта", "EPUB, FB2 и HTML — книга сверстается заново",
                 "Размер букв в книгах EPUB, FB2 и HTML. У PDF страница готовая — её увеличивают масштабом в книге.",
                 "%d пт", CH(ch_font), 0, 0 },
    [R_REFRESH] = { "full_refresh", "Полная перерисовка", "Полная перерисовка", "стирает следы прошлых страниц",
                    "Экран на миг мигает чёрным и стирается начисто. Чем реже — тем быстрее листание, но тем больше следов.",
                    NULL, CH(ch_refresh), 0, 0 },
    [R_HINT] = { "hint_shown", NULL, "Подсказка по жестам", "покажется один раз при открытии книги", NULL, NULL,
                 NULL, 0, 1, 1 },
    [R_CACHE] = { "cache_mb", "Кэш страниц", "Кэш страниц", "сколько готовых страниц держать в памяти",
                  "Готовые страницы лежат в памяти, чтобы листать назад и вперёд без ожидания. Больше кэш — "
                  "дольше помнит, но остаётся меньше памяти.",
                  "%d МБ", CH(ch_cache), 0, 0 },
    [R_AHEAD] = { "prefetch_ahead", "Страницы вперёд", "Готовить вперёд", "страниц, пока вы читаете текущую",
                  "Сколько страниц после текущей рисовать заранее, пока вы читаете. Листание вперёд тогда мгновенное.",
                  "%d стр.", CH(ch_ahead), 0, 0 },
    [R_BEHIND] = { "prefetch_behind", "Страницы назад", "Готовить назад", "страниц перед текущей",
                   "Сколько страниц перед текущей рисовать заранее — для листания назад.", "%d стр.", CH(ch_behind),
                   0, 0 },
    [R_BUSY] = { "screen_busy_ms", "Быстрое листание", "Пропуск страниц при быстром листании",
                 "если жмёте чаще, чем обновляется экран",
                 "Экран после обновления занят доли секунды. Нажатия за это время не показывают страницы по одной, "
                 "а сдвигают цель — покажется та, на которой остановились.",
                 "%d мс", CH(ch_busy), 0, 0 },
    [R_THREADS] = { "render_threads", "Потоки отрисовки", "Потоки отрисовки", "сколько страниц рисуется одновременно",
                    "У книги два ядра. Для EPUB, FB2 и HTML всегда один поток: каждый поток верстал бы всю книгу заново.",
                    "%d", CH(ch_threads), 0, 0 },
    [R_WAITING] = { "prefetch_while_waiting", NULL, "Соседние страницы пока ждёте",
                    "выкл — нужная страница рисуется быстрее", NULL, NULL, NULL, 0, 1, 0 },
    [R_STORE] = { "store_mb", "Кэш картинок и шрифтов", "Кэш картинок и шрифтов",
                  "уже разобранное не разбирать заново",
                  "Память, где держатся распакованные картинки и шрифты книги. Она нужна каждому потоку отрисовки "
                  "отдельно.",
                  "%d МБ", CH(ch_store), 0, 0 },
    [R_JPX] = { "jpx_min_scale", "Фото и сканы", "Фото и сканы: распаковка", "мельче — быстрее, чуть мягче",
                "Фон и фото в книгах из DjVu (формат JPEG 2000) можно распаковывать мельче нужного размера: "
                "на экране в 16 оттенков разницы почти не видно.",
                "%d %%", CH(ch_jpx), 0, 0 },
    [R_JPEG] = { "jpeg_gray", NULL, "Цветные JPEG сразу в серый", "до полутора раз быстрее на книгах с фото", NULL,
                 NULL, NULL, 0, 1, 0 },
    [R_ICC] = { "no_icc", NULL, "Упрощённый цвет в картинках", "быстрее на ~20 %, на сером экране незаметно", NULL,
                NULL, NULL, 0, 1, 0 },
};

struct settings {
    gfx *g;
    reader_options *opt;
    char config[1024];
    char launch[256];
    ui_rect rows[R_COUNT];       /* где строки на экране (для касаний) */
    ui_rect hit;
    int screen;                  /* SC_* */
    int parent;                  /* SC_*, откуда открыт выбор значения */
    int cur_item;                /* R_*, для которого открыт выбор */
    choice opts[MAX_CHOICES + 1];/* варианты на экране выбора */
    char custom_label[32];
    int nopts, cur_opt;
    ui_rect opt_rects[MAX_CHOICES + 1];
    int home;                    /* хаб — главный экран */
    ui_header_hits sub_hits;
    ui_rect stock_once, stock_forever;
    char root[1024];       /* корень библиотеки */
    char *dl;              /* буфер хаба: общая папка загрузок (полный путь) */
    size_t dl_cap;
    folderpick *fp;        /* выбор папки загрузок */
};

settings *settings_new(gfx *g, reader_options *opt, const char *config, int home)
{
    settings *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    st->home = home;
    st->g = g;
    st->opt = opt;
    snprintf(st->config, sizeof st->config, "%s", config);
    return st;
}

void settings_set_files(settings *st, const char *root, const char *hide, char *dl, size_t dl_cap,
                        void (*redraw)(void *user, int full), void *user)
{
    snprintf(st->root, sizeof st->root, "%s", root);
    st->dl = dl;
    st->dl_cap = dl_cap;
    folderpick_free(st->fp);
    st->fp = folderpick_new(st->g, root, hide, redraw, user);
}

void settings_free(settings *st)
{
    if (!st)
        return;
    folderpick_free(st->fp);
    free(st);
}

/* ---- Значения параметров ---- */

/* Значение так, как оно лежит в config.txt. */
static int get_cfg(const reader_options *o, int id)
{
    switch (id) {
    case R_FONT: return o->reflow_em > 0 ? o->reflow_em : 28;
    case R_REFRESH: return o->full_refresh_every;
    case R_HINT: return !o->show_hint;
    case R_CACHE: return (int)(o->cache_bytes >> 20);
    case R_AHEAD: return o->prefetch_ahead;
    case R_BEHIND: return o->prefetch_behind;
    case R_BUSY: return o->screen_busy_ms;
    case R_THREADS: return o->render_threads;
    case R_WAITING: return o->prefetch_while_waiting;
    case R_STORE: return (int)(o->doc.store_bytes >> 20);
    case R_JPX: return o->doc.jpx_min_scale > 0 && o->doc.jpx_min_scale < 100 ? o->doc.jpx_min_scale : 100;
    case R_JPEG: return !o->doc.rgb_jpeg;
    case R_ICC: return o->doc.no_icc;
    default: return 0;
    }
}

static void set_cfg(reader_options *o, int id, int v)
{
    switch (id) {
    case R_FONT: o->reflow_em = v; break;
    case R_REFRESH: o->full_refresh_every = v; break;
    case R_HINT: o->show_hint = !v; break;
    case R_CACHE: o->cache_bytes = (size_t)v << 20; break;
    case R_AHEAD: o->prefetch_ahead = v; break;
    case R_BEHIND: o->prefetch_behind = v; break;
    case R_BUSY: o->screen_busy_ms = v; break;
    case R_THREADS: o->render_threads = v; break;
    case R_WAITING: o->prefetch_while_waiting = v; break;
    case R_STORE: o->doc.store_bytes = (size_t)v << 20; break;
    case R_JPX: o->doc.jpx_min_scale = v; break;
    case R_JPEG: o->doc.rgb_jpeg = !v; break;
    case R_ICC: o->doc.no_icc = v; break;
    default: break;
    }
}

/* Переключатель включён? */
static int switch_on(const reader_options *o, int id)
{
    return items[id].inv ? !get_cfg(o, id) : get_cfg(o, id);
}

static void save_item(settings *st, int id)
{
    char v[32];
    snprintf(v, sizeof v, "%d", get_cfg(st->opt, id));
    int rc = config_save_value(st->config, items[id].key, v);
    log_msg("настройки: %s=%s%s", items[id].key, v, rc ? " — не удалось записать config.txt" : "");
}

/* Как называется текущее значение (для списка справа). */
static void value_label(const reader_options *o, int id, char *out, size_t len)
{
    const item *it = &items[id];
    int v = get_cfg(o, id);
    for (int i = 0; i < it->nch; i++)
        if (it->ch[i].value == v) {
            snprintf(out, len, "%s", it->ch[i].label);
            return;
        }
    snprintf(out, len, it->unit ? it->unit : "%d", v);
}

/* ---- Рисование ---- */

#define ROW_H 128
#define CHOICE_ROW_H 136

typedef enum { K_PLAIN, K_NAV, K_CHOICE, K_SWITCH } row_kind;

/* Переключатель вкл/выкл с подписью; right — правый край, cy — центр по вертикали. */
static void draw_switch(gfx *g, pf_screen *s, int right, int cy, int on)
{
    int w = 108, h = 60, x = right - w, d = 44;
    if (on) {
        gfx_rrect(g, s, x, cy - h / 2, w, h, h / 2.0f, UI_BLACK);
        gfx_rrect(g, s, x + w - d - 8, cy - d / 2, d, d, d / 2.0f, UI_WHITE);
    } else {
        gfx_rrect(g, s, x, cy - h / 2, w, h, h / 2.0f, UI_LINE);
        gfx_rrect(g, s, x + 8, cy - d / 2, d, d, d / 2.0f, UI_WHITE);
    }
    gfx_style ts = { FONT_MEDIUM, 32, on ? UI_BLACK : UI_TEXT3 };
    const char *t = on ? "Вкл" : "Выкл";
    gfx_text(g, s, x - 20 - gfx_text_width(g, ts, t), cy + 12, ts, t);
}

/* Строка карточки: название, подпись, справа — значение с «›», переключатель или «›». */
static void draw_row(settings *st, pf_screen *s, int x, int y, int w, const char *title, const char *sub,
                     row_kind kind, const char *value, int on, int line)
{
    gfx *g = st->g;
    int pad = 40, right = x + w - pad, cy = y + ROW_H / 2;
    if (kind == K_SWITCH) {
        draw_switch(g, s, right, cy, on);
        right -= 108 + 20 + 90; /* переключатель, отступ и «Выкл» */
    } else {
        if (kind == K_NAV || kind == K_CHOICE) {
            gfx_icon(g, s, ICON_NEXT, right - 40, cy - 20, 40, 0, UI_TEXT3);
            right -= 56;
        }
        if (value && *value)
            right = ui_chip(g, s, right, cy, value) - 24;
    }
    gfx_style ts = { FONT_REGULAR, 40, UI_BLACK }, ss = { FONT_REGULAR, 30, UI_TEXT2 };
    if (sub && *sub) {
        gfx_text_fit(g, s, x + pad, cy - 6, right - x - pad, ts, title);
        gfx_text_fit(g, s, x + pad, cy + 38, right - x - pad, ss, sub);
    } else {
        gfx_text_fit(g, s, x + pad, cy + 14, right - x - pad, ts, title);
    }
    if (line)
        gfx_fill(s, x + pad, y + ROW_H - 2, w - pad, 2, UI_FAINT);
}

/* Карточка со строками ids[0..n) от y; возвращает её низ. */
static int card(settings *st, pf_screen *s, int y, const char *caption, const int *ids, int n)
{
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    const reader_options *o = st->opt;
    if (caption) {
        ui_caption(st->g, s, x + 8, y + 40, caption);
        y += 58;
    }
    ui_card(st->g, s, x, y, w, n * ROW_H);
    for (int i = 0; i < n; i++) {
        int id = ids[i], ry = y + i * ROW_H;
        char val[64] = "";
        const char *title = NULL, *sub = NULL;
        row_kind kind = K_NAV;
        int on = 0;
        if (id < R_ITEMS) {
            title = items[id].title;
            sub = items[id].sub;
            on = switch_on(o, id);
            kind = items[id].sw ? K_SWITCH : K_CHOICE;
            if (kind == K_CHOICE)
                value_label(o, id, val, sizeof val);
        } else {
            switch (id) {
            case R_SPEED: title = "Скорость и память"; sub = "кэш, страницы заранее, потоки, пропуск страниц"; break;
            case R_IMAGES: title = "Картинки в книгах"; sub = "фото, сканы и цветные JPEG"; break;
            case R_SYSTEM: title = "Настройки прошивки"; sub = "Wi-Fi, сон, дата, язык"; break;
            case R_STATS: {
                int stats = pf_fw_statistics();
                title = "Статистика для PocketBook";
                sub = "что открывали и запускали — на серверы";
                kind = stats < 0 ? K_PLAIN : K_SWITCH;
                on = stats > 0;
                if (stats < 0)
                    snprintf(val, sizeof val, "?");
                break;
            }
            case R_DOWNLOADS: {
                char rel[1024];
                title = "Папка для загрузок";
                sub = "куда падают книги из сети";
                if (st->dl && dirpath_relative(st->root, st->dl, rel, sizeof rel) == 0) {
                    const char *slash = strrchr(rel, '/');
                    if (!*rel)
                        snprintf(val, sizeof val, "Вся библиотека");
                    else if (strlen(rel) > 24 && slash)
                        snprintf(val, sizeof val, "…%.40s", slash);
                    else
                        snprintf(val, sizeof val, "%.40s", rel);
                } else if (st->dl) {
                    const char *slash = strrchr(st->dl, '/');
                    snprintf(val, sizeof val, "%.40s", slash && slash[1] ? slash + 1 : st->dl);
                }
                break;
            }
            case R_QUIT: title = "Выход"; sub = "закрыть хаб"; kind = K_PLAIN; break;
            case R_STOCK: title = "Штатный главный экран"; sub = "вернуть без компьютера"; break;
            default: break;
            }
        }
        draw_row(st, s, x, ry, w, title, sub, kind, val, on, i + 1 < n);
        st->rows[id] = (ui_rect){ x, ry, w, ROW_H };
    }
    return y + n * ROW_H;
}

static void note(settings *st, pf_screen *s, int y, const char *text, int lines)
{
    gfx_style hs = { FONT_REGULAR, 30, UI_TEXT2 };
    gfx_text_wrap(st->g, s, UI_MARGIN + 8, y + 40, s->width - 2 * UI_MARGIN - 16, 40, lines, hs, text);
}

#define APPLY_NOTE "Применится, когда вы снова откроете книгу: открытая сейчас закроется, место чтения сохранится."

static void draw_main(settings *st, pf_screen *s, int top)
{
    ui_header(st->g, s, "Настройки", 0, 0, 1);
    /* Версия — справа на строке заголовка (внизу экрана места нет). */
    gfx_style vs = { FONT_REGULAR, 30, UI_TEXT3 };
    const char *ver = "версия " WASHI_VERSION;
    gfx_text(st->g, s, s->width - UI_MARGIN - gfx_text_width(st->g, vs, ver), UI_STATUS_H + 90, vs, ver);
    static const int reading[] = { R_FONT, R_REFRESH, R_HINT };
    int y = card(st, s, top - 16, "Чтение", reading, 3);
    static const int speed[] = { R_SPEED, R_IMAGES };
    y = card(st, s, y + 16, "Скорость", speed, 2);
    static const int dev[] = { R_SYSTEM, R_STATS, R_DOWNLOADS };
    y = card(st, s, y + 16, "Устройство", dev, st->dl ? 3 : 2);
    int last[1] = { st->home ? R_STOCK : R_QUIT };
    y = card(st, s, y + 40, NULL, last, 1);
    note(st, s, y + 20, "Подсветка и Wi-Fi — в быстрых настройках: коснитесь строки с часами вверху.", 2);
}

static void draw_speed(settings *st, pf_screen *s, int top)
{
    st->sub_hits = ui_header(st->g, s, "Скорость и память", 1, 0, 1);
    static const int pages[] = { R_CACHE, R_AHEAD, R_BEHIND, R_BUSY };
    int y = card(st, s, top - 16, "Страницы", pages, 4);
    static const int draw[] = { R_THREADS, R_WAITING, R_STORE };
    y = card(st, s, y + 16, "Отрисовка", draw, 3);
    note(st, s, y + 20, "Менять не обязательно: значения подобраны под этот ридер; меняйте, если листание тормозит или не хватает памяти. " APPLY_NOTE, 4);
}

static void draw_images(settings *st, pf_screen *s, int top)
{
    st->sub_hits = ui_header(st->g, s, "Картинки в книгах", 1, 0, 1);
    static const int photo[] = { R_JPX };
    int y = card(st, s, top - 16, "Фото и сканы", photo, 1);
    static const int color[] = { R_JPEG, R_ICC };
    y = card(st, s, y + 16, "Цвет", color, 2);
    note(st, s, y + 20,
         "Быстрее — значит чуть проще картинки; на 16 оттенках серого разницу почти не увидеть. "
         "«Сразу в серый» работает только вместе с упрощённым цветом. " APPLY_NOTE,
         6);
}

static void draw_stock(settings *st, pf_screen *s, int top)
{
    st->sub_hits = ui_header(st->g, s, "Штатный главный экран", 1, 0, 1);
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN, y = top + 20;
    gfx_style ts = { FONT_REGULAR, 36, UI_BLACK }, hs = { FONT_REGULAR, 30, UI_TEXT2 };
    y += 52 * gfx_text_wrap(st->g, s, x, y + 40, w, 52, 6, ts,
                            "Хаб сейчас — главный экран книги. Вернуть штатный можно здесь, без компьютера.");
    y += 60;
    ui_pill(st->g, s, x, y, w, 110, "До перезагрузки", 0);
    st->stock_once = (ui_rect){ x, y, w, 110 };
    gfx_text_wrap(st->g, s, x + 8, y + 160, w - 16, 40, 3, hs,
                  "Откроется штатный главный экран; после перезагрузки снова будет хаб.");
    y += 290;
    ui_pill(st->g, s, x, y, w, 110, "Насовсем", 1);
    st->stock_forever = (ui_rect){ x, y, w, 110 };
    gfx_text_wrap(st->g, s, x + 8, y + 160, w - 16, 40, 3, hs,
                  "Хаб перестанет быть главным экраном (файл system/bin/bookshelf.app удаляется). "
                  "Книги, места чтения и настройки хаба остаются.");
}

/* Выбор значения: рассказ, что это, и варианты с пояснениями; текущий отмечен. */
static void draw_choice(settings *st, pf_screen *s, int top)
{
    const item *it = &items[st->cur_item];
    gfx *g = st->g;
    st->sub_hits = ui_header(g, s, it->head, 1, 0, 1);
    int x = UI_MARGIN, w = s->width - 2 * UI_MARGIN;
    gfx_style hs = { FONT_REGULAR, 32, UI_TEXT2 };
    int y = top - 16;
    int lines = gfx_text_wrap(g, s, x + 8, y + 40, w - 16, 42, 4, hs, it->intro);
    y += 42 * lines + 36;
    ui_card(g, s, x, y, w, st->nopts * CHOICE_ROW_H);
    for (int i = 0; i < st->nopts; i++) {
        int ry = y + i * CHOICE_ROW_H, cy = ry + CHOICE_ROW_H / 2, sel = i == st->cur_opt;
        int mx = x + w - 40 - 44;
        gfx_rrect_line(g, s, mx, cy - 22, 44, 44, 22, 3, UI_BLACK);
        if (sel)
            gfx_rrect(g, s, mx + 10, cy - 12, 24, 24, 12, UI_BLACK);
        gfx_style ts = { sel ? FONT_MEDIUM : FONT_REGULAR, 40, UI_BLACK }, ss = { FONT_REGULAR, 30, UI_TEXT2 };
        int tw = mx - 24 - (x + 40);
        gfx_text_fit(g, s, x + 40, cy - 6, tw, ts, st->opts[i].label);
        gfx_text_fit(g, s, x + 40, cy + 38, tw, ss, st->opts[i].text);
        if (i + 1 < st->nopts)
            gfx_fill(s, x + 40, ry + CHOICE_ROW_H - 2, w - 40, 2, UI_FAINT);
        st->opt_rects[i] = (ui_rect){ x, ry, w, CHOICE_ROW_H };
    }
    note(st, s, y + st->nopts * CHOICE_ROW_H + 20, APPLY_NOTE, 3);
}

void settings_draw(settings *st, pf_screen *s)
{
    int top = ui_content_top(), bottom = ui_content_bottom(s);
    gfx_fill(s, 0, UI_STATUS_H, s->width, bottom - UI_STATUS_H, UI_BG);
    memset(st->rows, 0, sizeof st->rows);
    switch (st->screen) {
    case SC_SPEED: draw_speed(st, s, top); break;
    case SC_IMAGES: draw_images(st, s, top); break;
    case SC_CHOICE: draw_choice(st, s, top); break;
    case SC_STOCK: draw_stock(st, s, top); break;
    case SC_FOLDER: folderpick_draw(st->fp, s); break;
    default: draw_main(st, s, top); break;
    }
}

/* ---- Касания ---- */

/* Открыть выбор значения для строки id: варианты из таблицы, а если в config.txt вручную
 * записано что-то другое — это значение первым, чтобы его было видно и можно было оставить. */
static void open_choice(settings *st, int id)
{
    const item *it = &items[id];
    int v = get_cfg(st->opt, id), found = -1;
    st->nopts = 0;
    for (int i = 0; i < it->nch; i++)
        if (it->ch[i].value == v)
            found = i;
    if (found < 0) {
        snprintf(st->custom_label, sizeof st->custom_label, it->unit ? it->unit : "%d", v);
        st->opts[st->nopts++] = (choice){ v, st->custom_label, "Задано вручную в config.txt" };
    }
    for (int i = 0; i < it->nch && st->nopts < MAX_CHOICES + 1; i++)
        st->opts[st->nopts++] = it->ch[i];
    st->cur_opt = found < 0 ? 0 : found + (st->nopts - it->nch);
    st->cur_item = id;
    st->parent = st->screen;
    st->screen = SC_CHOICE;
}

static settings_action tap_choice(settings *st, int x, int y)
{
    for (int i = 0; i < st->nopts; i++)
        if (ui_inside(st->opt_rects[i], x, y)) {
            st->hit = st->opt_rects[i];
            st->screen = st->parent;
            if (i == st->cur_opt)
                return SETTINGS_REDRAW;
            set_cfg(st->opt, st->cur_item, st->opts[i].value);
            save_item(st, st->cur_item);
            return SETTINGS_CHANGED;
        }
    return SETTINGS_NONE;
}

settings_action settings_tap(settings *st, int x, int y, const pf_screen *s)
{
    if (y >= ui_content_bottom(s))
        return SETTINGS_NONE;
    if (st->screen == SC_FOLDER) {
        switch (folderpick_tap(st->fp, x, y)) {
        case FP_REDRAW: return SETTINGS_REDRAW;
        case FP_CANCEL:
            st->screen = SC_MAIN;
            return SETTINGS_REDRAW;
        case FP_PICKED: {
            const char *rel = folderpick_result(st->fp);
            char abs[1024];
            st->screen = SC_MAIN;
            if (dirpath_join(st->root, rel, abs, sizeof abs) == 0 && strlen(abs) < st->dl_cap) {
                snprintf(st->dl, st->dl_cap, "%s", abs);
                int rc = config_save_value(st->config, "download_dir", *rel ? rel : ".");
                log_msg("настройки: download_dir=%s%s", *rel ? rel : ".", rc ? " — не удалось записать config.txt" : "");
            }
            return SETTINGS_REDRAW;
        }
        default: return SETTINGS_NONE;
        }
    }
    if (st->screen != SC_MAIN && ui_inside(st->sub_hits.back, x, y)) {
        st->screen = st->screen == SC_CHOICE ? st->parent : SC_MAIN;
        return SETTINGS_REDRAW;
    }
    if (st->screen == SC_STOCK) {
        if (ui_inside(st->stock_once, x, y)) {
            st->hit = st->stock_once;
            return SETTINGS_STOCK_ONCE;
        }
        if (ui_inside(st->stock_forever, x, y)) {
            st->hit = st->stock_forever;
            return SETTINGS_STOCK_FOREVER;
        }
        return SETTINGS_NONE;
    }
    if (st->screen == SC_CHOICE)
        return tap_choice(st, x, y);

    int id = -1;
    for (int i = 0; i < R_COUNT; i++)
        if (ui_inside(st->rows[i], x, y)) {
            id = i;
            st->hit = st->rows[i];
        }
    if (id < 0)
        return SETTINGS_NONE;
    if (id < R_ITEMS) {
        if (!items[id].sw) {
            open_choice(st, id);
            return SETTINGS_REDRAW;
        }
        set_cfg(st->opt, id, !get_cfg(st->opt, id));
        save_item(st, id);
        return SETTINGS_CHANGED;
    }
    switch (id) {
    case R_SPEED:
        st->screen = SC_SPEED;
        return SETTINGS_REDRAW;
    case R_IMAGES:
        st->screen = SC_IMAGES;
        return SETTINGS_REDRAW;
    case R_SYSTEM:
        snprintf(st->launch, sizeof st->launch, "/ebrmain/bin/settings.app");
        return SETTINGS_LAUNCH;
    case R_STATS: {
        int on = pf_fw_statistics();
        if (on >= 0)
            pf_set_fw_statistics(on == 0);
        return SETTINGS_REDRAW;
    }
    case R_STOCK:
        st->screen = SC_STOCK;
        return SETTINGS_REDRAW;
    case R_DOWNLOADS: {
        char rel[1024] = "";
        if (!st->fp || !st->dl)
            return SETTINGS_NONE;
        if (dirpath_relative(st->root, st->dl, rel, sizeof rel) != 0)
            rel[0] = 0;
        folderpick_open(st->fp, "Папка для загрузок", rel);
        st->screen = SC_FOLDER;
        return SETTINGS_REDRAW;
    }
    case R_QUIT:
        return SETTINGS_QUIT;
    default:
        return SETTINGS_NONE;
    }
}

const char *settings_launch_path(const settings *st)
{
    return st->launch;
}

ui_rect settings_hit(const settings *st)
{
    return st->hit;
}

int settings_back(settings *st)
{
    if (st->screen == SC_MAIN)
        return 0;
    st->screen = SC_MAIN;
    return 1;
}
