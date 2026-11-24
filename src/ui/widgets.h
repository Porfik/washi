/* widgets — общий вид экранов хаба: размеры, оттенки, строка состояния, вкладки,
 * заголовок, строки списков.
 *
 * Экран 1404×1872, ~300 точек на дюйм (1 мм ≈ 12 точек). Оттенки — кратные 17, ровно
 * ложатся на 16 уровней e-ink без растра.
 */
#ifndef WASHI_UI_WIDGETS_H
#define WASHI_UI_WIDGETS_H

#include "gfx.h"

#define UI_MARGIN 72
#define UI_STATUS_H 96  /* время и заряд */
#define UI_HEADER_H 150 /* заголовок экрана под строкой состояния */
#define UI_TABS_H 168   /* вкладки внизу */
#define UI_ROW_H 144    /* строка списка */

#define UI_BLACK 0x00
#define UI_TEXT2 0x55 /* второстепенный текст */
#define UI_TEXT3 0x88 /* подписи, неактивное */
#define UI_LINE 0xcc  /* разделители, рамки */
#define UI_FAINT 0xee /* подложки */
#define UI_BG 0xee    /* фон экранов с карточками (как в Stoic): белые карточки на светло-сером */
#define UI_WHITE 0xff
#define UI_CARD_R 30  /* скругление карточек */

typedef struct {
    int x, y, w, h;
} ui_rect;

int ui_inside(ui_rect r, int x, int y);

typedef enum { TAB_BOOKS, TAB_LIBRARY, TAB_APPS, TAB_SETTINGS, TAB_COUNT } ui_tab;

/* Строка состояния: время слева, заряд справа. */
void ui_status(gfx *g, pf_screen *s);

/* Вкладки внизу экрана; active — выбранная. */
void ui_tabs(gfx *g, pf_screen *s, int active);

/* Вкладка под точкой; -1 — не на вкладках. */
int ui_tab_at(const pf_screen *s, int x, int y);

/* Заголовок экрана. back — стрелка «назад» слева. pages > 1 — справа «‹ 2 / 5 ›».
 * Возвращает области касания (нулевые, если их нет). */
typedef struct {
    ui_rect back, prev, next;
} ui_header_hits;
ui_header_hits ui_header(gfx *g, pf_screen *s, const char *title, int back, int page, int pages);

/* Где начинается и кончается содержимое экрана (между заголовком и вкладками). */
int ui_content_top(void);
int ui_content_bottom(const pf_screen *s);

/* Подпись раздела (мелко, серым) с baseline y. */
void ui_caption(gfx *g, pf_screen *s, int x, int y, const char *text);

/* Полоса прогресса: дорожка и заполненная часть (frac 0..1). */
void ui_progress(gfx *g, pf_screen *s, int x, int y, int w, int h, float frac);

/* Белая скруглённая карточка на сером фоне. */
void ui_card(gfx *g, pf_screen *s, int x, int y, int w, int h);

/* Значение-«чип»: светлая пилюля с текстом, правый край — right, по вертикали — центр cy.
 * Возвращает левый край. */
int ui_chip(gfx *g, pf_screen *s, int right, int cy, const char *text);

/* Кнопка-пилюля: filled — чёрная с белым текстом, иначе — белая с рамкой. */
void ui_pill(gfx *g, pf_screen *s, int x, int y, int w, int h, const char *label, int filled);

/* Строка внутри карточки (x..x+w): название, подпись, справа чип со значением или «›». */
void ui_card_row(gfx *g, pf_screen *s, int x, int y, int w, int h, const char *title, const char *sub,
                 const char *value, int chevron, int line);

/* Отклик на касание: чёрная рамка по области r, быстрым обновлением (до тяжёлого действия —
 * открытия книги, запуска приложения), чтобы было видно, что касание принято. */
void ui_flash(gfx *g, pf_screen *s, ui_rect r);

/* Ползунок во всю ширину экрана. ui_slider_x — левый край дорожки и её ширина (*w);
 * ui_slider_track — дорожка с ручкой, frac 0..1, центр по вертикали cy;
 * ui_slider_percent — значение 0..100 под точкой x;
 * ui_slider_row — строка высотой h от y: подпись, «NN %» справа, ползунок. */
int ui_slider_x(const pf_screen *s, int *w);
void ui_slider_track(gfx *g, pf_screen *s, int x, int cy, int w, float frac);
int ui_slider_percent(const pf_screen *s, int x);
void ui_slider_row(gfx *g, pf_screen *s, int y, int h, const char *label, int percent);

/* Размер файла: «812 КБ», «3,4 МБ», «25 МБ»; size < 0 — пустая строка. */
void ui_format_size(char *buf, size_t len, long long size);

/* Путь в «хлебные крошки»: первые n байт rel, «/» → « / ». */
void ui_path_crumbs(const char *rel, size_t n, char *out, size_t len);

/* Склонение: 1 книга, 2 книги, 5 книг (forms — «книга», «книги», «книг»). */
const char *ui_plural(int n, const char *one, const char *few, const char *many);

#endif
