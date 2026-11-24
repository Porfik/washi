/* app — хаб целиком: вкладки «Книги» (home), «Библиотека» (library), «Приложения» (apps),
 * «Настройки» (settings) и открытая книга (reader). PDF открываются своей читалкой, другие
 * форматы (EPUB, DjVu…) — штатной (pf_open_book).
 *
 * При запуске — вкладка «Книги» (или книга из командной строки). Из книги кнопка меню →
 * «Главная» возвращает в хаб, книга остаётся открытой: карточка «Продолжить чтение» или
 * кнопка меню в хабе возвращают к ней сразу из кэша. Где остановились — positions.txt.
 *
 * Плитки «Приём по Wi‑Fi», «Каталоги», «Синхронизация» во вкладке «Приложения» открывают
 * страницы хаба (page.h) поверх неё.
 */
#ifndef WASHI_UI_APP_H
#define WASHI_UI_APP_H

#include "reader.h"

typedef struct {
    reader_options reader;    /* reader.path — книга из командной строки или NULL */
    const char *library_root; /* где искать книги */
    const char *home_fails;   /* хаб — главный экран: файл счётчика неудачных запусков
                                 (обнуляется после первого показа), иначе NULL */
    const char *home_path;    /* хаб — главный экран: откуда запущен (подмена прошивки) */
    const char *stock_once;   /* файл-просьба: при следующем запуске — штатный главный экран */
    const char *download_dir; /* куда класть книги, принятые по сети; NULL — <library_root>/Downloads */
} app_options;

typedef struct app app;

app *app_new(const app_options *opt);
void app_free(app *a);

/* Обработчик событий платформы: pf_run(app_on_event, a). */
void app_on_event(const pf_event *ev, void *user);

#endif
