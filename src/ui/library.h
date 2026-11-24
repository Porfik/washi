/* Вкладка «Библиотека»: все книги устройства.
 *
 * Под заголовком — фильтры «Все · Читаю · Новые · Прочитано», справа — сортировка и вид
 * (касание открывает лист снизу: «Обложки / Список / Папки», «Недавние / Название /
 * Добавлены») и поиск по названию. По умолчанию — сетка обложек; «Папки» — дерево папок
 * с PDF для тех, кто раскладывает книги сам. Вид и сортировка запоминаются в config.txt
 * (lib_view, lib_sort). Касание книги — открыть, папки — войти; кнопки, свайпы и стрелки
 * в заголовке листают страницы.
 */
#ifndef WASHI_UI_LIBRARY_H
#define WASHI_UI_LIBRARY_H

#include "../base/positions.h"
#include "../doc/covers.h"
#include "../platform/platform.h"
#include "gfx.h"
#include "widgets.h"

typedef struct library library;

typedef enum {
    LIBRARY_NONE,
    LIBRARY_REDRAW, /* изменился экран: перерисовать */
    LIBRARY_OPEN,   /* открыть книгу library_selected() */
    LIBRARY_FOLDER, /* долгое касание папки: действия с папкой library_folder() */
    LIBRARY_SEARCH, /* спросить строку поиска (клавиатура) и передать в library_search */
} library_action;

/* root — где искать книги; config_path — куда запоминать вид и сортировку.
 * g, pos и cv должны жить дольше библиотеки. */
library *library_new(const char *root, gfx *g, const positions *pos, covers *cv, const char *config_path);
void library_free(library *l);

/* Книги на устройстве изменились (приняли по сети, удалили, переименовали): найти заново при
 * следующем показе; открытая папка остаётся открытой, если она ещё есть. */
void library_reload(library *l);

/* Заголовок и книги (при первом показе — найти книги). current — открытая книга или NULL. */
void library_draw(library *l, pf_screen *s, const char *current);

/* Дорисовать обложки, которые были не готовы при library_draw; возвращает, сколько ещё ждём. */
int library_covers_ready(library *l, pf_screen *s);

library_action library_tap(library *l, int x, int y);
library_action library_key(library *l, pf_key key);

/* Шаг назад: закрыть лист, выйти из поиска, на папку выше. 0 — уже в начале. */
int library_back(library *l);

/* Закрыть лист вида и сортировки (уходя с вкладки). */
void library_close_sheet(library *l);

/* Показать книги, в названии которых есть query (без учёта регистра и «ё»). */
void library_search(library *l, const char *query);
const char *library_query(const library *l);

/* Долгое касание: книга — LIBRARY_OPEN (карточка), папка в виде «Папки» — LIBRARY_FOLDER (не входит),
 * остальное — как обычное касание. */
library_action library_long(library *l, int x, int y);

/* Папка для LIBRARY_FOLDER: путь, имя и сколько в ней книг (с вложенными папками). */
const char *library_folder(const library *l);
const char *library_folder_name(const library *l);
int library_folder_books(const library *l);

/* Книга для LIBRARY_OPEN и её место на экране (для отклика рамкой). */
const char *library_selected(const library *l);
ui_rect library_hit(const library *l);

#endif
