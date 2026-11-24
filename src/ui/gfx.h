/* gfx — рисование интерфейса в буфер экрана: текст шрифтами прошивки (Roboto), фигуры
 * и иконки со сглаживанием (растеризатор MuPDF), картинки в оттенках серого.
 *
 * Шрифты ищутся в $WASHI_FONTS, /ebrmain/fonts и /mnt/ext1/system/fonts; нет ни одного
 * из списка — встроенный Helvetica MuPDF (Nimbus Sans). Только для потока интерфейса.
 */
#ifndef WASHI_UI_GFX_H
#define WASHI_UI_GFX_H

#include <stdint.h>

#include "../platform/platform.h"

typedef struct gfx gfx;

typedef enum { FONT_REGULAR, FONT_MEDIUM, FONT_BOLD, FONT_LIGHT, FONT_COUNT } gfx_font;

typedef struct {
    gfx_font font;
    float size; /* кегль, пикселей */
    uint8_t gray;
} gfx_style;

/* Значки: смысл → контур Lucide (scripts/icons.py, порядок тот же). */
typedef enum {
    ICON_BOOK,
    ICON_FOLDER,
    ICON_APPS,
    ICON_SETTINGS,
    ICON_TOC,
    ICON_BACK,
    ICON_NEXT,
    ICON_SEARCH,
    ICON_BOOKMARK,
    ICON_WIFI,
    ICON_WIFI_OFF,
    ICON_FIT_PAGE,
    ICON_SUN,
    ICON_LIBRARY,
    ICON_DOWNLOAD, /* приём по Wi‑Fi */
    ICON_CATALOG,  /* каталоги OPDS */
    ICON_SYNC,     /* синхронизация папки */
    ICON_PLUS,
    ICON_CALC, /* дальше — значки приложений */
    ICON_CALENDAR,
    ICON_NOTES,
    ICON_DRAW,
    ICON_IMAGE,
    ICON_GLOBE,
    ICON_CHESS,
    ICON_SUDOKU,
    ICON_CARDS,
    ICON_DICTIONARY,
    ICON_PUZZLE,
    ICON_APP,
    ICON_COUNT
} gfx_icon_id;

gfx *gfx_new(void);
void gfx_free(gfx *g);

/* Строка UTF-8 от (x, baseline); возвращает ширину. s == NULL — только ширина. */
int gfx_text(gfx *g, pf_screen *s, int x, int baseline, gfx_style st, const char *str);
int gfx_text_width(gfx *g, gfx_style st, const char *str);

/* В ширину max_w: не влезает — обрезается с «…». Возвращает ширину. */
int gfx_text_fit(gfx *g, pf_screen *s, int x, int baseline, int max_w, gfx_style st, const char *str);

/* То же, но «…» посередине: конец (расширение файла, хвост адреса) остаётся виден —
 * «Очень длинное наз…ние книги.pdf». Возвращает ширину. */
int gfx_text_fit_middle(gfx *g, pf_screen *s, int x, int baseline, int max_w, gfx_style st, const char *str);

/* С переносом по словам, не больше max_lines строк (последняя — с «…», если текст не влез),
 * шаг строк line_h. Возвращает число строк. s == NULL — только посчитать. */
int gfx_text_wrap(gfx *g, pf_screen *s, int x, int baseline, int max_w, int line_h, int max_lines,
                  gfx_style st, const char *str);
/* То же, строки по центру ширины max_w. */
int gfx_text_wrap_center(gfx *g, pf_screen *s, int x, int baseline, int max_w, int line_h, int max_lines,
                         gfx_style st, const char *str);

/* Прямоугольник без сглаживания (быстро). */
void gfx_fill(pf_screen *s, int x, int y, int w, int h, uint8_t gray);

/* Скруглённый прямоугольник: залитый и контур толщиной line. */
void gfx_rrect(gfx *g, pf_screen *s, int x, int y, int w, int h, float r, uint8_t gray);
void gfx_rrect_line(gfx *g, pf_screen *s, int x, int y, int w, int h, float r, float line, uint8_t gray);

/* Значок в квадрате size×size с левым верхним углом (x, y).
 * line — толщина линии в точках (0 — как задумано: 2 на 24). */
void gfx_icon(gfx *g, pf_screen *s, gfx_icon_id id, int x, int y, int size, float line, uint8_t gray);

/* То же; filled — линия толще (залитых вариантов у Lucide нет). */
void gfx_icon_ex(gfx *g, pf_screen *s, gfx_icon_id id, int x, int y, int size, float line, int filled,
                 uint8_t gray);

/* Контур SVG (path d) в квадрате, как значок: viewBox vb, заливка или линия толщиной line. */
void gfx_svg(gfx *g, pf_screen *s, const char *d, const float vb[4], int x, int y, int size, float line,
             uint8_t gray);

/* Картинка 8 бит серого (w×h, шаг stride) в прямоугольник dw×dh с усреднением (уменьшение)
 * или повтором точек (увеличение). */
void gfx_image(pf_screen *s, int x, int y, int dw, int dh, const uint8_t *img, int w, int h, int stride);

/* Скруглить углы прямоугольника на экране радиусом r (со сглаживанием) цветом фона bg. */
void gfx_round_corners(pf_screen *s, int x, int y, int w, int h, int r, uint8_t bg);

#endif
