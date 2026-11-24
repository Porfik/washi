/* page — экран хаба, открытый из плитки во вкладке «Приложения», со своим заголовком
 * «‹ Название»: приём книг по Wi‑Fi (recv.c), каталоги OPDS (opds.c), синхронизация с папкой
 * на сервере (sync.c).
 *
 * Хаб (app.c) рисует строку состояния и вкладки, страница — всё между ними. Пока страница на
 * экране, хаб раз в PAGE_TICK_MS зовёт tick: забрать готовое из своих фоновых потоков, показать
 * ход передачи (можно самой перерисовать часть экрана и вызвать pf_update — как обложки).
 * Все функции зовутся только из потока интерфейса; сеть — в своих потоках страницы.
 */
#ifndef WASHI_UI_PAGE_H
#define WASHI_UI_PAGE_H

#include "../base/positions.h"
#include "../doc/covers.h"
#include "../platform/platform.h"
#include "gfx.h"

#define PAGE_TICK_MS 500

/* Что сделать хабу после tap, key или tick (флаги, можно несколько). */
enum {
    PAGE_REDRAW = 1, /* перерисовать экран целиком */
    PAGE_CLOSE = 2,  /* закрыть страницу (назад — во вкладку «Приложения») */
    PAGE_BOOKS = 4,  /* книги на устройстве изменились: библиотеке найти их заново */
    PAGE_OPEN = 8,   /* открыть книгу selected() (страница закроется) */
};

typedef struct page page;

typedef struct {
    void (*open)(page *p);  /* страницу показывают (касание плитки) */
    void (*close)(page *p); /* с неё уходят: вкладка, «Домой», книга (останавливать серверы) */
    void (*draw)(page *p, pf_screen *s);
    int (*tap)(page *p, int x, int y);
    int (*key)(page *p, pf_key key); /* ‹ › и свайпы влево-вправо; меню и «Домой» — у хаба */
    int (*tick)(page *p, pf_screen *s);
    const char *(*selected)(page *p); /* книга для PAGE_OPEN */
    void (*free)(page *p);
    /* Хаб хочет уйти со страницы (вкладка, «Домой», кнопка меню). 0 — можно сразу. 1 — идёт
     * передача: страница рисует вопрос «Остановить?» (хаб тут же вызовет draw) и потом сама
     * вернёт из tap PAGE_CLOSE — остановить и уйти (хаб выполнит отложенный переход) — или
     * PAGE_REDRAW — остаться. Своя «‹» в заголовке спрашивает так же, без хаба. NULL — 0. */
    int (*leave)(page *p);
} page_ops;

struct page {
    const page_ops *ops;
    int gray; /* фон серый (белые карточки, как «Настройки»), иначе белый */
};

/* Общее для страниц; всё живёт дольше страниц. */
typedef struct {
    gfx *g;
    covers *cv;
    positions *pos;           /* места чтения хаба: менять только в потоке интерфейса, потом positions_save */
    const char *library_root; /* где книги («/mnt/ext1») */
    const char *download_dir; /* куда класть принятые и скачанные книги (может ещё не существовать) */
    const char *config_path;  /* config.txt хаба (config_load, config_save_value) */
    const char *data_dir;     /* папка хаба (pf_data_dir) */
    /* Нарисовать экран заново (full — с миганием): после pf_keyboard и pf_net_connect, когда
     * поверх был экран прошивки. Зовётся из потока интерфейса. */
    void (*redraw)(void *user, int full);
    void *user;
} page_env;

/* Страницы. NULL — не хватило памяти. */
page *recv_page_new(const page_env *env); /* приём по Wi‑Fi: страница в браузере и FTP */
page *opds_page_new(const page_env *env); /* каталоги OPDS */
page *sync_page_new(const page_env *env); /* синхронизация с папкой WebDAV */

#endif
