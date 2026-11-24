/* Вкладка «Приложения»: дополнения хаба, установленные приложения PocketBook и встроенные
 * приложения прошивки. Касание — запустить; хаб уходит в фон и возвращается после.
 *
 * Дополнение хаба — папка <папка данных>/apps/ИМЯ/ с файлом app.txt:
 *     name=Название
 *     exec=программа.app       (путь от папки дополнения или от корня)
 * и, по желанию, icon.svg — значок (контуры path; fill="none" у <svg> — линии, как Lucide).
 * Так можно добавлять свои программы, не трогая сам хаб.
 *
 * Первые плитки — свои экраны хаба (приём по Wi‑Fi, каталоги, синхронизация): их путь
 * начинается с «hub:», и хаб открывает их сам, а не запускает.
 */
#ifndef WASHI_UI_APPS_H
#define WASHI_UI_APPS_H

#include "../platform/platform.h"
#include "gfx.h"
#include "widgets.h"

#define APPS_HUB_RECV "hub:recv"
#define APPS_HUB_OPDS "hub:opds"
#define APPS_HUB_SYNC "hub:sync"

typedef struct apps apps;

typedef enum { APPS_NONE, APPS_REDRAW, APPS_LAUNCH } apps_action;

apps *apps_new(gfx *g);
void apps_free(apps *a);

/* Перечитать список (при входе во вкладку). */
void apps_refresh(apps *a);

void apps_draw(apps *a, pf_screen *s);
apps_action apps_tap(apps *a, int x, int y);
apps_action apps_key(apps *a, pf_key key);

/* Программа для APPS_LAUNCH и её название. */
const char *apps_selected(const apps *a);
const char *apps_selected_name(const apps *a);

/* Плитка выбранного приложения (для отклика рамкой). */
ui_rect apps_hit(const apps *a);

#endif
