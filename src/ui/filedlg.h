/* filedlg — вопрос или сообщение карточкой по центру на очищенном фоне (удалить книгу или папку,
 * имя занято, действия с папкой). Две кнопки — рядом, три — столбиком. Безопасная кнопка — чёрная,
 * опасная («Удалить») — белая. */
#ifndef WASHI_UI_FILEDLG_H
#define WASHI_UI_FILEDLG_H

#include "gfx.h"
#include "widgets.h"

#define FILEDLG_MAX_BTN 3

typedef struct {
    int open;
    int kind; /* что спрашиваем — на усмотрение владельца */
    char title[400];
    char text[640];
    char label[FILEDLG_MAX_BTN][48];
    int filled[FILEDLG_MAX_BTN];
    int n;
    ui_rect hit[FILEDLG_MAX_BTN];
} filedlg;

/* Показать вопрос: n кнопок с подписями label[] и «чёрностью» filled[] (1 — чёрная). */
void filedlg_show(filedlg *d, int kind, const char *title, const char *text, int n, const char *const *label,
                  const int *filled);

/* Нарисовать поверх экрана хаба (очищает область между строкой состояния и вкладками). */
void filedlg_draw(filedlg *d, gfx *g, pf_screen *s);

/* Номер нажатой кнопки или -1. */
int filedlg_tap(const filedlg *d, int x, int y);

#endif
