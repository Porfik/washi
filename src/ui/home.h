/* home — вкладка «Книги»: карточка книги, которую читают (обложка, автор, прогресс),
 * и недавние книги списком с маленькими обложками. */
#ifndef WASHI_UI_HOME_H
#define WASHI_UI_HOME_H

#include <stddef.h>

#include "../base/positions.h"
#include "../doc/covers.h"
#include "gfx.h"
#include "widgets.h"

typedef struct home home;

/* pos, cv, g должны жить дольше home. root — корень библиотеки (для подписи папки). */
home *home_new(gfx *g, const positions *pos, covers *cv, const char *root);
void home_free(home *h);

/* Содержимое экрана между строкой состояния и вкладками (экран не обновляет). */
void home_draw(home *h, pf_screen *s);

/* Дорисовать обложки, которые были не готовы при home_draw, и обновить их на экране.
 * Возвращает, сколько ещё ждём. text_changed — появились авторы: перерисовать экран. */
int home_covers_ready(home *h, pf_screen *s, int *text_changed);

/* Касание: 1 — открыть книгу (путь в path, область касания в hit), 0 — мимо. */
int home_tap(home *h, int x, int y, char *path, size_t len, ui_rect *hit);

/* Хвосты, которые убирать из названий («(источник)|(скан)», без учёта регистра): config.txt,
 * title_strip. Зовётся при запуске. */
void home_set_title_strip(const char *list);
/* Название книги по имени файла: без расширения, без хвостов title_strip, «_» → пробел. */
void home_book_title(const char *path, char *out, size_t len);

#endif
