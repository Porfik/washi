/* folderpick — выбор папки внутри библиотеки (library_root): путь «хлебными крошками» с «‹» —
 * выше, список подпапок (скрытые, папки устройства и хаба не видны), касание — войти, «Новая
 * папка» (клавиатура прошивки), «Выбрать эту папку». Показывается на весь экран между
 * заголовком и вкладками — внутри «Настроек» и страниц хаба.
 *
 * Выше корня библиотеки не выйти, «..» и ссылки наружу не принимаются (base/dirpath.h).
 *
 * dirsel — готовое «куда сохранять» для приёма по Wi‑Fi и каталогов: «Общая папка загрузок»
 * или «Другая папка…» (folderpick); выбор лежит в своём файле data_dir, пусто — общая.
 */
#ifndef WASHI_UI_FOLDERPICK_H
#define WASHI_UI_FOLDERPICK_H

#include <stddef.h>

#include "../platform/platform.h"
#include "gfx.h"

/* Как называется папка rel (от корня) на экране: «Вся библиотека» или «Книги / Новые». */
void folder_label(const char *rel, char *out, size_t len);

typedef struct folderpick folderpick;

typedef enum {
    FP_NONE,
    FP_REDRAW, /* перерисовать экран */
    FP_PICKED, /* выбрана папка — folderpick_result() */
    FP_CANCEL, /* закрыли без выбора */
} fp_result;

/* root — корень библиотеки; hide — полный путь папки, которую не показывать (данные хаба; может быть NULL).
 * redraw(user, 1) зовётся после клавиатуры прошивки («Новая папка»), когда поверх был её экран. */
folderpick *folderpick_new(gfx *g, const char *root, const char *hide, void (*redraw)(void *user, int full), void *user);
void folderpick_free(folderpick *fp);

/* Открыть с папки start (путь от корня; нет такой — ближайшая существующая выше). */
void folderpick_open(folderpick *fp, const char *title, const char *start);

void folderpick_draw(folderpick *fp, pf_screen *s);
fp_result folderpick_tap(folderpick *fp, int x, int y);

/* Выбранная папка — путь от корня библиотеки ("" — сам корень); после FP_PICKED. */
const char *folderpick_result(const folderpick *fp);

/* ---- «Куда сохранять»: общая папка или своя ---- */

typedef struct dirsel dirsel;

typedef enum {
    DS_NONE,
    DS_REDRAW,
    DS_CHANGED, /* выбор сменился (и записан в файл) */
} ds_result;

/* shared — общая папка загрузок (полный путь; указатель на живой буфер хаба, его читают при каждом
 * обращении); file — где хранится выбор. */
dirsel *dirsel_new(gfx *g, const char *root, const char *hide, const char *shared, const char *file,
                   void (*redraw)(void *user, int full), void *user);
void dirsel_free(dirsel *ds);

void dirsel_load(dirsel *ds);                      /* прочитать выбор из файла (при открытии страницы) */
int dirsel_custom(const dirsel *ds);               /* 1 — своя папка, 0 — общая */
void dirsel_dir(const dirsel *ds, char *out, size_t len);   /* куда класть: полный путь */
void dirsel_label(const dirsel *ds, char *out, size_t len); /* «Downloads (общая)» или «Книги / Новые» */

void dirsel_start(dirsel *ds); /* показать выбор «Общая / Другая папка…» */
int dirsel_active(const dirsel *ds);
void dirsel_draw(dirsel *ds, pf_screen *s); /* пока dirsel_active: весь экран между строкой состояния и вкладками */
ds_result dirsel_tap(dirsel *ds, int x, int y);

#endif
