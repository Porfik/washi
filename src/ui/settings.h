/* Вкладка «Настройки»: чтение, скорость и память, картинки, настройки и статистика прошивки,
 * выход. Строка с несколькими значениями открывает экран выбора (варианты с пояснениями,
 * текущий отмечен), переключатель вкл/выкл меняется касанием; выбранное сразу пишется
 * в config.txt, а книге передаётся при следующем открытии (SETTINGS_CHANGED). Если хаб —
 * главный экран, вместо «Выхода» — «Штатный главный экран»: вернуть штатный до перезагрузки
 * или насовсем прямо с книги, без компьютера. */
#ifndef WASHI_UI_SETTINGS_H
#define WASHI_UI_SETTINGS_H

#include <stddef.h>

#include "../platform/platform.h"
#include "gfx.h"
#include "reader.h"
#include "widgets.h"

typedef struct settings settings;

typedef enum {
    SETTINGS_NONE,
    SETTINGS_REDRAW,
    SETTINGS_CHANGED, /* изменились параметры книги: открытую книгу открыть заново */
    SETTINGS_LAUNCH,  /* запустить settings_launch_path() */
    SETTINGS_QUIT,
    SETTINGS_STOCK_ONCE,    /* штатный главный экран до перезагрузки */
    SETTINGS_STOCK_FOREVER, /* убрать хаб с главного экрана */
} settings_action;

/* opt — параметры программы (меняются на месте), config — путь к config.txt;
 * home — хаб запущен главным экраном. */
settings *settings_new(gfx *g, reader_options *opt, const char *config, int home);

/* Папка для загрузок: root — корень библиотеки, hide — папка данных хаба (не показывать), dl — буфер
 * хаба с полным путём общей папки загрузок (меняется сразу, на него смотрят страницы), redraw —
 * перерисовать экран после клавиатуры прошивки. Выбор пишется в config.txt (download_dir). */
void settings_set_files(settings *st, const char *root, const char *hide, char *dl, size_t dl_cap,
                        void (*redraw)(void *user, int full), void *user);
void settings_free(settings *st);

void settings_draw(settings *st, pf_screen *s);
settings_action settings_tap(settings *st, int x, int y, const pf_screen *s);
const char *settings_launch_path(const settings *st);

/* Открыт ли вложенный экран; settings_back — вернуться на главный экран настроек (за один раз). */
int settings_back(settings *st);

/* Строка, которой коснулись последней (для отклика рамкой). */
ui_rect settings_hit(const settings *st);

#endif
