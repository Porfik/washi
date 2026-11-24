/* doc — обёртка над MuPDF: открыть файл, размеры страниц, отрисовка в 8-битный серый буфер.
 *
 * Не потокобезопасна: документ вместе со своим fz_context в каждый момент используется
 * одним потоком (открыть можно в одном, рисовать в другом). Из любого потока можно
 * звать только doc_cookie_abort и doc_mem_stats.
 */
#ifndef WASHI_DOC_H
#define WASHI_DOC_H

#include <stddef.h>
#include <stdint.h>

typedef struct doc doc;

/* Прямоугольник в пунктах (1/72 дюйма), в координатах страницы MuPDF. */
typedef struct {
    float x0, y0, x1, y1;
} doc_rect;

/* Как страница ложится в буфер: пиксель (px, py) — это точка страницы
 * (x0 + px / scale, y0 + py / scale). */
typedef struct {
    float scale; /* пикселей на пункт */
    float x0, y0;
} doc_view;

typedef struct {
    size_t store_bytes; /* лимит кэша ресурсов MuPDF (картинки, шрифты); 0 — без лимита */
    int aa_level;       /* бит сглаживания 0..8; -1 — по умолчанию MuPDF */
    int no_icc;         /* 1 — без ICC-профилей (быстрее преобразование цвета) */
    int jpx_no_reduce;  /* 1 — JPX всегда в полном размере (патч MuPDF 0002 выключен) */
    int jpx_threads;    /* потоков распаковки JPX; 0 — по умолчанию (2) */
    int no_fast_paths;  /* 1 — без быстрых путей патча MuPDF 0003 (для сравнения) */
    int mupdf_upscale;  /* 1 — увеличивать картинки, как MuPDF: попиксельно при наложении */
    int jpx_min_scale;  /* JPX можно распаковать мельче нужного, до этого процента (0 — 100 %) */
    int jpx_locked;     /* 1 — JPX под общей блокировкой MuPDF, как без патча 0004 */
    int slow_abort;     /* 1 — отмена только между командами страницы, как без патча 0006 */
    int no_neon;        /* 1 — без NEON: масштабирование и RGB → серый (патч 0007) */
    int flate_check;    /* 1 — проверять Adler-32 в конце данных Flate, как MuPDF (патч 0007) */
    int eager_clip;     /* 1 — маска отсечения сразу на всю область, как MuPDF (без патча 0013) */
    int rgb_jpeg;       /* 1 — цветной JPEG через RGB, как MuPDF; 0 — сразу серый из яркости
                           (патч 0008, только с no_icc) */
    int reflow_w, reflow_h; /* вёрстка «перетекающих» книг (EPUB/FB2/HTML) в пунктах;
                               0 — A4 595×842. У читалки — экран: страница ложится в буфер
                               в масштабе 1, без увеличения и обрезки */
    int reflow_em;      /* кегль вёрстки «перетекающих» книг; 0 — по умолчанию (28) */
} doc_options;

/* Отмена отрисовки из другого потока. Сбрасывать (doc_cookie_reset) до начала отрисовки,
 * под той же блокировкой, под которой другой поток решает, звать ли doc_cookie_abort. */
typedef struct doc_cookie doc_cookie;

doc_cookie *doc_cookie_new(void);
void doc_cookie_free(doc_cookie *c);
void doc_cookie_reset(doc_cookie *c);
void doc_cookie_abort(doc_cookie *c);

typedef struct {
    double load_ms;   /* загрузка страницы (fz_load_page + fz_bound_page) */
    double render_ms; /* отрисовка (fz_run_page в пиксмап) */
} doc_timing;

/* Вписать страницу целиком в w×h пикселей по центру. */
doc_view doc_fit_page(const doc_rect *r, int w, int h);

doc *doc_open(const char *path, const doc_options *opt, char *err, size_t err_len);

/* Открывает ли этот формат наша читалка (по расширению; сейчас — только PDF). Остальные
 * книги хаб отдаёт штатной читалке. */
int doc_supported(const char *path);

/* Перетекающая ли книга (EPUB/FB2/HTML — после doc_open; для вёрстки fz_layout_document). */
int doc_reflowable(const doc *d);
void doc_close(doc *d);

/* Переключатели патчей MuPDF общие для процесса и ставятся при открытии документа.
 * Чтобы рисовать по очереди документами с разными doc_options (бенчмарк, --compare),
 * перед отрисовкой поставить переключатели этого документа. Не во время отрисовки
 * в других потоках. */
void doc_apply_switches(const doc *d);

int doc_page_count(const doc *d);

/* Пункт оглавления: заголовок, страница (с 0; -1 — ссылка не на страницу), вложенность с 0. */
typedef struct {
    char title[200];
    int page;
    int level;
} doc_outline_item;

/* Оглавление плоским списком в порядке книги. Возвращает число пунктов (0 — оглавления нет);
 * *out освобождать free(). */
int doc_outline(doc *d, doc_outline_item **out);

/* Ссылка на странице: прямоугольник (точки страницы) и страница, куда ведёт (с 0). */
typedef struct {
    doc_rect rect;
    int page;
} doc_link;

/* Внутренние ссылки страницы (наружные — адреса сайтов — пропускаются). Возвращает число;
 * *out освобождать free(). */
int doc_links(doc *d, int page, doc_link **out);

/* Сведения о документе по ключу MuPDF (FZ_META_INFO_TITLE = "info:Title", "info:Author"…).
 * 0 — есть (buf заполнен, обрезан пробелами), -1 — нет или пусто. */
int doc_meta(doc *d, const char *key, char *buf, size_t len);

/* Границы страницы (номер с 0). Загруженная страница остаётся в памяти до загрузки другой. */
int doc_page_bounds(doc *d, int page, doc_rect *out, doc_timing *t);

/* Рисует страницу в буфер dst (w×h, шаг строки stride байт), фон белый.
 * cookie может быть NULL. Возвращает 0, 1 — прервано через cookie, -1 — ошибка (doc_error). */
int doc_render(doc *d, int page, const doc_view *view, uint8_t *dst, int w, int h, int stride,
               doc_cookie *cookie, doc_timing *t);

const char *doc_error(const doc *d);

/* Предупреждения MuPDF с момента открытия документа. */
int doc_warning_count(const doc *d);

/* Память, выделенная MuPDF (все документы): сейчас и пик, байты. */
void doc_mem_stats(size_t *current, size_t *peak);

/* Начать отсчёт пика заново с текущего значения. */
void doc_mem_reset_peak(void);

#endif
