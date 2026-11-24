#include "app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "../base/bookfiles.h"
#include "../base/clock.h"
#include "../base/config.h"
#include "../base/dirpath.h"
#include "../base/log.h"
#include "../base/positions.h"
#include "../base/secret.h"
#include "../base/sysinfo.h"
#include "../doc/covers.h"
#include "../doc/doc.h"
#include "apps.h"
#include "bookinfo.h"
#include "filedlg.h"
#include "gesture.h"
#include "home.h"
#include "library.h"
#include "page.h"
#include "quick.h"
#include "settings.h"
#include "widgets.h"

#define FULL_EVERY 12      /* полное обновление раз в столько показов экранов хаба */
#define COVER_POLL_MS 300 /* как часто проверять готовые обложки, пока они рисуются */

enum { PENDING_HOME = 100, PENDING_BOOK }; /* a->pending, кроме номеров вкладок */

/* Страницы хаба (page.h), по путям плиток «hub:…» из apps.h. */
enum { PAGE_RECV, PAGE_OPDS, PAGE_SYNC, PAGE_COUNT };
static const struct {
    const char *path;
    page *(*make)(const page_env *env);
} page_kinds[PAGE_COUNT] = {
    { APPS_HUB_RECV, recv_page_new },
    { APPS_HUB_OPDS, opds_page_new },
    { APPS_HUB_SYNC, sync_page_new },
};

struct app {
    app_options opt;
    char cmdline_file[1024];
    char library_root[1024];
    char config_path[1024];
    char home_fails[1024]; /* счётчик неудачных запусков главного экрана; "" — не главный экран */
    int is_home;           /* хаб — главный экран */
    int home_signalled;    /* монитору сказано «главный экран запущен» */
    char home_path[1024], stock_once[1024];
    positions pos;
    gfx *g;
    covers *cv;
    home *home;
    library *library;
    apps *apps;
    settings *settings;
    bookinfo *info; /* карточка книги поверх вкладки (долгое касание книги) */
    int info_open;
    filedlg dlg;          /* вопрос или сообщение по центру (удаление, переименование) */
    char fop_path[1024]; /* книга или папка, с которой работает вопрос или клавиатура */
    quick *qs; /* быстрые настройки (шторка сверху) */
    int quick_open;
    page *pages[PAGE_COUNT]; /* страницы хаба (создаются при первом открытии) */
    int page;                /* открытая страница, -1 — нет */
    int pending;             /* куда уйти, когда страница остановит передачу: вкладка, PENDING_*, -1 */
    char download_dir[1024];
    page_env penv;
    reader *reader; /* открытая книга или NULL */
    int in_reader;  /* книга на экране */
    int tab;
    int shows;
    gesture_state gs;
};

app *app_new(const app_options *opt)
{
    app *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    a->opt = *opt;
    snprintf(a->cmdline_file, sizeof a->cmdline_file, "%s", opt->reader.path ? opt->reader.path : "");
    snprintf(a->library_root, sizeof a->library_root, "%s", opt->library_root);
    snprintf(a->config_path, sizeof a->config_path, "%s/config.txt", pf_data_dir());
    snprintf(a->home_fails, sizeof a->home_fails, "%s", opt->home_fails ? opt->home_fails : "");
    a->is_home = opt->home_fails != NULL;
    snprintf(a->home_path, sizeof a->home_path, "%s", opt->home_path ? opt->home_path : "");
    snprintf(a->stock_once, sizeof a->stock_once, "%s", opt->stock_once ? opt->stock_once : "");
    /* download_dir в config.txt: путь от корня библиотеки («Downloads», «Книги/Новые») или полный */
    if (dirpath_config(opt->library_root, opt->download_dir, a->download_dir, sizeof a->download_dir) != 0)
        snprintf(a->download_dir, sizeof a->download_dir, "%s/Downloads", opt->library_root);
    a->page = -1;
    a->pending = -1;
    char path[1024];
    snprintf(path, sizeof path, "%s/positions.txt", pf_data_dir());
    if (positions_load(&a->pos, path) < 0) {
        free(a);
        return NULL;
    }
    a->tab = TAB_BOOKS;
    return a;
}

void app_free(app *a)
{
    if (!a)
        return;
    reader_free(a->reader);
    for (int i = 0; i < PAGE_COUNT; i++)
        if (a->pages[i])
            a->pages[i]->ops->free(a->pages[i]);
    home_free(a->home);
    library_free(a->library);
    apps_free(a->apps);
    settings_free(a->settings);
    bookinfo_free(a->info);
    quick_free(a->qs);
    covers_free(a->cv);
    gfx_free(a->g);
    positions_free(&a->pos);
    free(a);
}

/* Запомнить, где остановились в открытой книге. */
static void save_position(app *a)
{
    if (!a->reader || reader_page_count(a->reader) <= 0)
        return;
    positions_set(&a->pos, reader_path(a->reader), reader_page(a->reader), reader_page_count(a->reader));
    if (positions_save(&a->pos) != 0)
        log_msg("не удалось записать %s", a->pos.file);
}

static void close_book(app *a)
{
    if (!a->reader)
        return;
    save_position(a);
    double t0 = clock_ms();
    reader_free(a->reader);
    log_msg("закрыта книга: %.1f ms", clock_ms() - t0);
    a->reader = NULL;
    a->in_reader = 0;
}

/* ---- Экраны хаба ---- */

/* Первый экран показан — запуск главного экрана удался: счётчик неудач в ноль
 * (main.c, home_failsafe). */
static void home_started(app *a)
{
    if (a->is_home && !a->home_signalled) {
        a->home_signalled = 1;
        pf_home_started(); /* монитор запустит диспетчер задач: «Домой», окно USB */
    }
    static int logged;
    if (!logged) { /* что работает на устройстве — один раз, после первого показа (не задерживая его) */
        logged = 1;
        sysinfo_log("запуск");
    }
    if (!*a->home_fails)
        return;
    FILE *f = fopen(a->home_fails, "w");
    if (f) {
        fputs("0\n", f);
        fclose(f);
    }
    a->home_fails[0] = 0;
}

static void draw_hub(app *a, int full)
{
    pf_screen *s = pf_get_screen();
    if (!s || !a->g)
        return;
    double t0 = clock_ms();
    const char *current = a->reader && reader_page_count(a->reader) > 0 ? reader_path(a->reader) : NULL;
    page *pg = a->page >= 0 ? a->pages[a->page] : NULL;
    /* «Приложения» и «Настройки» — карточки на сером фоне, остальные — на белом. */
    int gray = pg ? pg->gray : !a->info_open && (a->tab == TAB_APPS || a->tab == TAB_SETTINGS);
    gfx_fill(s, 0, 0, s->width, UI_STATUS_H, gray ? UI_BG : UI_WHITE);
    ui_status(a->g, s);
    if (pg)
        pg->ops->draw(pg, s);
    else if (a->info_open)
        bookinfo_draw(a->info, s);
    else
        switch (a->tab) {
    case TAB_BOOKS: home_draw(a->home, s); break;
    case TAB_LIBRARY: library_draw(a->library, s, current); break;
    case TAB_APPS: apps_draw(a->apps, s); break;
    case TAB_SETTINGS: settings_draw(a->settings, s); break;
    }
    if (a->dlg.open)
        filedlg_draw(&a->dlg, a->g, s);
    ui_tabs(a->g, s, a->tab);
    double t1 = clock_ms();
    a->shows++;
    pf_update_mode mode = full || a->shows % FULL_EVERY == 0 ? PF_UPDATE_FULL_HQ : PF_UPDATE_PARTIAL;
    pf_update(0, 0, s->width, s->height, mode);
    log_msg("хаб: вкладка %d%s, отрисовка %.1f ms, %s", a->tab,
            pg ? " (страница)" : a->info_open ? " (карточка книги)" : "", t1 - t0, pf_update_mode_name(mode));
    home_started(a);
    if (pg)
        pf_set_aux_timer(PAGE_TICK_MS);
    else if ((a->tab == TAB_BOOKS || a->tab == TAB_LIBRARY || a->info_open) && covers_pending(a->cv))
        pf_set_aux_timer(COVER_POLL_MS);
}

/* ---- Страницы хаба ---- */

static void close_page(app *a)
{
    if (a->page < 0)
        return;
    page *pg = a->pages[a->page];
    a->page = -1;
    pg->ops->close(pg);
}

static void page_redraw(void *user, int full)
{
    app *a = user;
    if (!a->in_reader)
        draw_hub(a, full);
}

/* Плитка «hub:…» — открыть страницу. 0 — это была страница. */
static int open_page(app *a, const char *path)
{
    for (int i = 0; i < PAGE_COUNT; i++) {
        if (strcmp(path, page_kinds[i].path))
            continue;
        if (!a->pages[i]) {
            a->penv = (page_env){ a->g, a->cv, &a->pos, a->library_root, a->download_dir, a->config_path,
                                  pf_data_dir(), page_redraw, a };
            a->pages[i] = page_kinds[i].make(&a->penv);
        }
        if (!a->pages[i]) {
            log_msg("хаб: не хватило памяти на страницу %s", path);
            return 0;
        }
        log_msg("хаб: страница %s", path);
        a->page = i;
        a->pages[i]->ops->open(a->pages[i]);
        draw_hub(a, 0);
        return 0;
    }
    return -1;
}

static void open_any(app *a, const char *path, double t);
static void open_book(app *a, const char *path, double t);
static void go_home(app *a);

/* Уйти со страницы к target (вкладка или PENDING_*). 1 — страница занята передачей и спросила
 * «Остановить?»: переход отложен до её ответа. */
static int page_busy(app *a, int target)
{
    if (a->page < 0)
        return 0;
    page *pg = a->pages[a->page];
    if (!pg->ops->leave || !pg->ops->leave(pg))
        return 0;
    a->pending = target;
    draw_hub(a, 0);
    return 1;
}

/* Отложенный переход после «Остановить» (страница уже закрыта). */
static void run_pending(app *a, double t)
{
    int target = a->pending;
    a->pending = -1;
    if (target == PENDING_HOME) {
        go_home(a);
    } else if (target == PENDING_BOOK && a->reader && reader_page_count(a->reader) > 0) {
        open_book(a, reader_path(a->reader), t);
    } else {
        if (target >= 0 && target < TAB_COUNT)
            a->tab = target;
        if (a->tab == TAB_APPS)
            apps_refresh(a->apps);
        draw_hub(a, 0);
    }
}

/* Что попросила страница (флаги PAGE_*). from_tap — ответ на касание: «остаться» в вопросе
 * «Остановить?» снимает отложенный переход (ход передачи из tick — нет). */
static void page_result(app *a, int r, double t, int from_tap)
{
    if (from_tap && !(r & (PAGE_CLOSE | PAGE_OPEN)))
        a->pending = -1;
    if (a->page < 0 || !r)
        return;
    page *pg = a->pages[a->page];
    if (r & PAGE_BOOKS)
        library_reload(a->library);
    if (r & PAGE_OPEN) {
        char path[1024];
        const char *sel = pg->ops->selected(pg);
        snprintf(path, sizeof path, "%s", sel ? sel : "");
        close_page(a);
        if (*path) {
            open_any(a, path, t);
            return;
        }
        draw_hub(a, 0);
        return;
    }
    if (r & PAGE_CLOSE) {
        close_page(a);
        if (a->pending >= 0)
            run_pending(a, t);
        else
            draw_hub(a, 0);
        return;
    }
    if (r & PAGE_REDRAW)
        draw_hub(a, 0);
}

static void to_hub(app *a, int tab, int full)
{
    close_page(a);
    save_position(a);
    if (a->reader && a->in_reader)
        reader_pause(a->reader);
    a->in_reader = 0;
    a->tab = tab;
    a->info_open = 0;
    a->quick_open = 0;
    pf_set_orientation(0); /* хаб — всегда книжный */
    covers_pause(a->cv, 0);
    draw_hub(a, full);
}

static void open_book(app *a, const char *path, double t)
{
    close_page(a);
    char book[1024];
    snprintf(book, sizeof book, "%s", path); /* path может указывать в список мест */
    if (a->reader && !strcmp(book, reader_path(a->reader)) && reader_page_count(a->reader) > 0) {
        covers_pause(a->cv, 1);
        a->in_reader = 1;
        pf_set_orientation(reader_current_options(a->reader)->orient);
        reader_resume(a->reader, t);
        return;
    }
    close_book(a);
    covers_pause(a->cv, 1);
    const position *p = positions_find(&a->pos, book);
    reader_options o = a->opt.reader;
    o.path = book;
    o.start_page = p ? p->page : 0;
    o.view = p ? p->view : 0;
    o.has_crop = p ? p->has_crop : 0;
    if (p && p->has_crop)
        memcpy(o.crop, p->crop, sizeof o.crop);
    o.orient = p ? p->orient : 0;
    o.spread = p ? p->spread : 0;
    o.contrast = p ? p->contrast : 0;
    log_msg("открываю %s со стр. %d", book, o.start_page + 1);
    pf_set_orientation(o.orient); /* до reader_new: страницы рисуются по размеру экрана */
    a->reader = reader_new(&o);
    if (!a->reader) {
        to_hub(a, a->tab, 0);
        return;
    }
    a->in_reader = 1;
    pf_event ev = { 0 };
    ev.type = PF_EVENT_START;
    ev.time_ms = t;
    reader_on_event(&ev, a->reader);
    save_position(a); /* книга — первой в недавних */
    ev.type = PF_EVENT_SHOW;
    reader_on_event(&ev, a->reader);
    home_started(a);
}

/* Книга: PDF — своей читалкой, остальное — штатной (хаб уходит в фон и вернётся). */
static void open_any(app *a, const char *path, double t)
{
    if (doc_supported(path)) {
        open_book(a, path, t);
        return;
    }
    log_msg("открываю штатной читалкой: %s", path);
    save_position(a);
    covers_pause(a->cv, 1);
    if (pf_open_book(path) != 0) {
        log_msg("не открылась: %s", path);
        covers_pause(a->cv, 0);
        draw_hub(a, 0); /* убрать рамку-отклик */
    }
}

static void launch(app *a, const char *path, const char *name)
{
    if (open_page(a, path) == 0)
        return;
    log_msg("запуск %s (%s)", path, name);
    save_position(a);
    covers_pause(a->cv, 1);
    if (pf_launch(path) != 0) {
        log_msg("не запустилось: %s", path);
        covers_pause(a->cv, 0);
        draw_hub(a, 0); /* убрать рамку-отклик */
    }
}

static void on_start(app *a, double t)
{
    secret_init(); /* ключ паролей — из прошивки, в потоке интерфейса */
    a->g = gfx_new();
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/covers", pf_data_dir());
    a->cv = covers_new(dir, &a->opt.reader.doc);
    if (a->g) {
        a->home = home_new(a->g, &a->pos, a->cv, a->library_root);
        a->library = library_new(a->library_root, a->g, &a->pos, a->cv, a->config_path);
        a->apps = apps_new(a->g);
        a->settings = settings_new(a->g, &a->opt.reader, a->config_path, a->is_home);
        if (a->settings)
            settings_set_files(a->settings, a->library_root, pf_data_dir(), a->download_dir, sizeof a->download_dir,
                               page_redraw, a);
        a->info = bookinfo_new(a->g, a->cv, &a->pos, a->library_root);
        a->qs = quick_new(a->g);
    }
    if (!a->g || !a->cv || !a->home || !a->library || !a->apps || !a->settings || !a->info || !a->qs)
        log_msg("хаб: не хватило памяти");
    if (*a->cmdline_file)
        open_any(a, a->cmdline_file, t); /* экран нарисует PF_EVENT_SHOW */
}

/* ---- Книга ---- */

/* Просьбы книги после события. */
static void handle_requests(app *a, double t)
{
    if (!a->reader)
        return;
    int req = reader_take_requests(a->reader);
    if (!req)
        return;
    const reader_options *o = reader_current_options(a->reader);
    if (req & READER_SAVE_BOOK) {
        save_position(a);
        positions_set_view(&a->pos, reader_path(a->reader), o->view, o->has_crop ? o->crop : NULL);
        positions_set_display(&a->pos, reader_path(a->reader), o->orient, o->spread, o->contrast);
        if (positions_save(&a->pos) != 0)
            log_msg("не удалось записать %s", a->pos.file);
    }
    if (req & READER_FINISHED) {
        const position *p = positions_find(&a->pos, reader_path(a->reader));
        if (p && !p->finished) {
            save_position(a);
            positions_set_finished(&a->pos, reader_path(a->reader), 1);
            positions_save(&a->pos);
            log_msg("дочитана: %s", reader_path(a->reader));
        }
    }
    if (req & READER_HINT_SHOWN) {
        a->opt.reader.show_hint = 0;
        config_save_value(a->config_path, "hint_shown", "1");
    }
    if (req & READER_WANT_REOPEN) {
        char path[1024];
        snprintf(path, sizeof path, "%s", reader_path(a->reader));
        close_book(a);
        open_book(a, path, t);
    } else if (req & READER_WANT_LIBRARY) {
        to_hub(a, TAB_BOOKS, 1);
    }
}

/* ---- Поиск книг ---- */

static void search_done(const char *text, void *user)
{
    app *a = user;
    if (text)
        library_search(a->library, text);
    draw_hub(a, 1); /* после клавиатуры — полностью, без следов */
}

static void ask_search(app *a)
{
    pf_keyboard("Поиск книги по названию", library_query(a->library), 100, search_done, a);
}

/* ---- Карточка книги ---- */

static void open_info(app *a, const char *path, ui_rect hit)
{
    pf_screen *s = pf_get_screen();
    if (s && hit.w)
        ui_flash(a->g, s, hit);
    log_msg("карточка книги: %s", path);
    bookinfo_open(a->info, path);
    a->info_open = 1;
    draw_hub(a, 0);
}


/* ---- Переименовать и удалить книгу или папку ---- */

enum { DLG_BOOK_DEL, DLG_DIR_MENU, DLG_DIR_DEL, DLG_MSG };

static void bookmarks_file(char *out, size_t len)
{
    snprintf(out, len, "%s/bookmarks.txt", pf_data_dir());
}

/* Открытая книга лежит в path (или, если dir, под ним) — закрыть до удаления или переименования. */
static void close_book_in(app *a, const char *path, int dir)
{
    if (!a->reader)
        return;
    const char *rp = reader_path(a->reader);
    size_t n = strlen(path);
    if (dir ? !strncmp(rp, path, n) && rp[n] == '/' : !strcmp(rp, path))
        close_book(a);
}

static void show_message(app *a, const char *title, const char *text, int full)
{
    static const int filled[1] = { 1 };
    const char *const label[1] = { "Понятно" };
    filedlg_show(&a->dlg, DLG_MSG, title, text, 1, label, filled);
    draw_hub(a, full);
}

static void ask_delete_book(app *a, const char *path)
{
    char title[400], stem[300];
    snprintf(a->fop_path, sizeof a->fop_path, "%s", path);
    bookfiles_stem(path, stem, sizeof stem);
    snprintf(title, sizeof title, "Удалить «%s»?", stem);
    static const int filled[2] = { 0, 1 };
    const char *const label[2] = { "Удалить", "Отмена" };
    filedlg_show(&a->dlg, DLG_BOOK_DEL, title, "Книга пропадёт с устройства. Это необратимо.", 2, label, filled);
    draw_hub(a, 0);
}

static void ask_dir_menu(app *a)
{
    snprintf(a->fop_path, sizeof a->fop_path, "%s", library_folder(a->library));
    int n = library_folder_books(a->library);
    char title[400], text[100];
    snprintf(title, sizeof title, "Папка «%s»", library_folder_name(a->library));
    snprintf(text, sizeof text, "%d %s", n, ui_plural(n, "книга", "книги", "книг"));
    static const int filled[3] = { 0, 0, 1 };
    const char *const label[3] = { "Переименовать", "Удалить", "Закрыть" };
    filedlg_show(&a->dlg, DLG_DIR_MENU, title, text, 3, label, filled);
    draw_hub(a, 0);
}

static void ask_delete_dir(app *a)
{
    int n = library_folder_books(a->library);
    char title[400], text[300];
    snprintf(title, sizeof title, "Удалить папку «%s»?", library_folder_name(a->library));
    snprintf(text, sizeof text, "Пропадёт всё, что в ней лежит, в том числе %d %s. Это необратимо.", n,
             ui_plural(n, "книга", "книги", "книг"));
    static const int filled[2] = { 0, 1 };
    const char *const label[2] = { "Удалить", "Отмена" };
    filedlg_show(&a->dlg, DLG_DIR_DEL, title, text, 2, label, filled);
    draw_hub(a, 0);
}

static void book_gone_or_moved(app *a)
{
    library_reload(a->library);
}

/* Новое имя книги введено (fop_path — книга). */
static void rename_book_done(const char *text, void *user)
{
    app *a = user;
    if (!text) {
        draw_hub(a, 1);
        return;
    }
    char old[1024], now[1024], bm[1100];
    snprintf(old, sizeof old, "%s", a->fop_path);
    close_book_in(a, old, 0);
    bookmarks_file(bm, sizeof bm);
    bf_result r = bookfiles_rename_book(a->library_root, &a->pos, bm, a->cv, old, text, now, sizeof now);
    if (r == BF_OK) {
        book_gone_or_moved(a);
        if (a->info_open)
            bookinfo_open(a->info, now);
    }
    if (r == BF_OK || r == BF_SAME)
        draw_hub(a, 1);
    else
        show_message(a, "Не переименовалось", bookfiles_message(r), 1);
}

static void rename_dir_done(const char *text, void *user)
{
    app *a = user;
    if (!text) {
        draw_hub(a, 1);
        return;
    }
    char now[1024], bm[1100];
    close_book_in(a, a->fop_path, 1);
    bookmarks_file(bm, sizeof bm);
    bf_result r = bookfiles_rename_dir(a->library_root, &a->pos, bm, a->cv, a->fop_path, text, now, sizeof now);
    if (r == BF_OK)
        book_gone_or_moved(a);
    if (r != BF_OK && r != BF_SAME)
        show_message(a, "Не переименовалось", bookfiles_message(r), 1);
    else
        draw_hub(a, 1);
}

static void ask_rename_book(app *a, const char *path)
{
    char stem[600];
    snprintf(a->fop_path, sizeof a->fop_path, "%s", path);
    bookfiles_stem(path, stem, sizeof stem);
    pf_keyboard("Новое имя книги", stem, 200, rename_book_done, a);
}

static void ask_rename_dir(app *a)
{
    pf_keyboard("Новое имя папки", library_folder_name(a->library), 200, rename_dir_done, a);
}

/* Ответ на вопрос или сообщение по центру. */
static void dlg_tap(app *a, int x, int y)
{
    int b = filedlg_tap(&a->dlg, x, y);
    if (b < 0)
        return;
    int kind = a->dlg.kind;
    a->dlg.open = 0;
    char bm[1100];
    bookmarks_file(bm, sizeof bm);
    switch (kind) {
    case DLG_BOOK_DEL:
        if (b == 0) {
            char path[1024];
            snprintf(path, sizeof path, "%s", a->fop_path);
            close_book_in(a, path, 0);
            bf_result r = bookfiles_delete_book(a->library_root, &a->pos, bm, a->cv, path);
            a->info_open = 0;
            book_gone_or_moved(a);
            if (r != BF_OK)
                show_message(a, "Не удалилось", bookfiles_message(r), 0);
            else
                draw_hub(a, 0);
            return;
        }
        break;
    case DLG_DIR_MENU:
        if (b == 0) {
            ask_rename_dir(a);
            return;
        }
        if (b == 1) {
            ask_delete_dir(a);
            return;
        }
        break;
    case DLG_DIR_DEL:
        if (b == 0) {
            close_book_in(a, a->fop_path, 1);
            bf_result r = bookfiles_delete_dir(a->library_root, &a->pos, bm, a->cv, a->fop_path);
            book_gone_or_moved(a);
            if (r != BF_OK)
                show_message(a, "Не всё удалилось", bookfiles_message(r), 0);
            else
                draw_hub(a, 0);
            return;
        }
        break;
    default: break;
    }
    draw_hub(a, 0);
}

static void info_tap(app *a, int x, int y, double t)
{
    pf_screen *s = pf_get_screen();
    char path[1024];
    ui_rect hit;
    snprintf(path, sizeof path, "%s", bookinfo_path(a->info));
    switch (bookinfo_tap(a->info, x, y, &hit)) {
    case BOOKINFO_BACK:
        a->info_open = 0;
        draw_hub(a, 0);
        break;
    case BOOKINFO_READ:
        if (s)
            ui_flash(a->g, s, hit);
        a->info_open = 0;
        open_any(a, path, t);
        break;
    case BOOKINFO_START: {
        if (s)
            ui_flash(a->g, s, hit);
        if (a->reader && !strcmp(reader_path(a->reader), path))
            close_book(a);
        const position *p = positions_find(&a->pos, path);
        if (doc_supported(path)) {
            positions_set(&a->pos, path, 0, p ? p->count : 0);
            positions_save(&a->pos);
        }
        a->info_open = 0;
        open_any(a, path, t);
        break;
    }
    case BOOKINFO_REMOVE:
        if (a->reader && !strcmp(reader_path(a->reader), path))
            close_book(a);
        positions_remove(&a->pos, path);
        if (positions_save(&a->pos) != 0)
            log_msg("не удалось записать %s", a->pos.file);
        log_msg("убрана из недавних: %s", path);
        a->info_open = 0;
        draw_hub(a, 0);
        break;
    case BOOKINFO_RENAME:
        if (s)
            ui_flash(a->g, s, hit);
        ask_rename_book(a, path);
        break;
    case BOOKINFO_DELETE:
        if (s)
            ui_flash(a->g, s, hit);
        ask_delete_book(a, path);
        break;
    case BOOKINFO_NONE:
        break;
    }
}

/* Долгое касание книги — её карточка. */
static void hub_long(app *a, const gesture *g)
{
    pf_screen *s = pf_get_screen();
    if (!s || a->info_open || a->dlg.open || a->page >= 0 || ui_tab_at(s, g->x, g->y) >= 0)
        return;
    char path[1024];
    ui_rect hit;
    if (a->tab == TAB_BOOKS) {
        if (home_tap(a->home, g->x, g->y, path, sizeof path, &hit))
            open_info(a, path, hit);
    } else if (a->tab == TAB_LIBRARY) {
        switch (library_long(a->library, g->x, g->y)) {
        case LIBRARY_OPEN: open_info(a, library_selected(a->library), library_hit(a->library)); break;
        case LIBRARY_FOLDER:
            ui_flash(a->g, s, library_hit(a->library));
            ask_dir_menu(a);
            break;
        case LIBRARY_REDRAW: draw_hub(a, 0); break;
        case LIBRARY_SEARCH: ask_search(a); break;
        case LIBRARY_NONE: break;
        }
    }
}

/* ---- События хаба ---- */

static void hub_key(app *a, const pf_event *ev)
{
    if (a->dlg.open) {
        a->dlg.open = 0; /* любая кнопка — «Отмена» */
        draw_hub(a, 0);
        return;
    }
    if (a->info_open) {
        if (ev->key == PF_KEY_MENU) {
            a->info_open = 0;
            draw_hub(a, 0);
        }
        return;
    }
    if (ev->key == PF_KEY_MENU) {
        /* Меню — к открытой книге (как «Продолжить»). */
        if (a->reader && reader_page_count(a->reader) > 0 && !page_busy(a, PENDING_BOOK))
            open_book(a, reader_path(a->reader), ev->time_ms);
        return;
    }
    if (a->page >= 0) {
        page *pg = a->pages[a->page];
        page_result(a, pg->ops->key(pg, ev->key), ev->time_ms, 1);
        return;
    }
    int redraw = 0;
    if (a->tab == TAB_LIBRARY)
        redraw = library_key(a->library, ev->key) == LIBRARY_REDRAW;
    else if (a->tab == TAB_APPS)
        redraw = apps_key(a->apps, ev->key) == APPS_REDRAW;
    if (redraw)
        draw_hub(a, 0);
}

/* Вернуть штатный главный экран без компьютера. Хаб закрывается, монитор прошивки запускает
 * главный экран заново: «насовсем» — подмены уже нет, и он берёт штатный; «до перезагрузки» —
 * снова запускается этот файл и по просьбе stock_once сразу передаёт место штатному (main.c). */
static void stock_home(app *a, int forever)
{
    save_position(a);
    if (forever) {
        int rc = *a->home_path ? remove(a->home_path) : -1;
        log_msg("штатный главный экран насовсем: удалить %s — %s", a->home_path, rc == 0 ? "да" : "не вышло");
        if (rc != 0)
            return;
    } else {
        FILE *f = fopen(a->stock_once, "w");
        if (f)
            fclose(f);
        log_msg("штатный главный экран до перезагрузки: %s", f ? a->stock_once : "не удалось записать просьбу");
        if (!f)
            return;
    }
    log_sync();
    pf_quit();
}

static void hub_tap(app *a, const gesture *g, double t)
{
    pf_screen *s = pf_get_screen();
    if (!s)
        return;
    if (a->dlg.open) {
        dlg_tap(a, g->x, g->y);
        return;
    }
    int tab = ui_tab_at(s, g->x, g->y);
    if (tab >= 0) {
        if (tab != ui_tab_at(s, g->x1, g->y1))
            return;
        if (a->info_open || a->page >= 0) {
            if (page_busy(a, tab))
                return;
            close_page(a); /* вкладка поверх страницы — в эту вкладку (и «Приложения» — к плиткам) */
            a->info_open = 0;
            if (tab == TAB_APPS)
                apps_refresh(a->apps);
            a->tab = tab;
            draw_hub(a, 0);
            return;
        }
        if (tab == a->tab) {
            /* Повторное касание вкладки — в её начало (без поиска, корень папок, главный экран настроек). */
            if (tab == TAB_LIBRARY && library_back(a->library)) {
                while (library_back(a->library))
                    ;
                draw_hub(a, 0);
            } else if (tab == TAB_SETTINGS && settings_back(a->settings)) {
                draw_hub(a, 0);
            }
            return;
        }
        if (a->tab == TAB_LIBRARY)
            library_close_sheet(a->library);
        a->tab = tab;
        if (tab == TAB_APPS)
            apps_refresh(a->apps);
        draw_hub(a, 0);
        return;
    }
    if (a->info_open) {
        info_tap(a, g->x, g->y, t);
        return;
    }
    if (a->page >= 0) {
        page *pg = a->pages[a->page];
        page_result(a, pg->ops->tap(pg, g->x, g->y), t, 1);
        return;
    }
    char path[1024];
    ui_rect hit;
    int x = g->x, y = g->y;
    switch (a->tab) {
    case TAB_BOOKS:
        if (home_tap(a->home, x, y, path, sizeof path, &hit)) {
            ui_flash(a->g, s, hit);
            open_book(a, path, t);
        }
        break;
    case TAB_LIBRARY:
        switch (library_tap(a->library, x, y)) {
        case LIBRARY_REDRAW: draw_hub(a, 0); break;
        case LIBRARY_OPEN:
            ui_flash(a->g, s, library_hit(a->library));
            open_any(a, library_selected(a->library), t);
            break;
        case LIBRARY_SEARCH: ask_search(a); break;
        case LIBRARY_FOLDER:
        case LIBRARY_NONE: break;
        }
        break;
    case TAB_APPS:
        switch (apps_tap(a->apps, x, y)) {
        case APPS_REDRAW: draw_hub(a, 0); break;
        case APPS_LAUNCH:
            ui_flash(a->g, s, apps_hit(a->apps));
            launch(a, apps_selected(a->apps), apps_selected_name(a->apps));
            break;
        case APPS_NONE: break;
        }
        break;
    case TAB_SETTINGS: {
        settings_action sa = settings_tap(a->settings, x, y, s);
        switch (sa) {
        case SETTINGS_CHANGED:
            /* Открытая книга откроется заново с новыми параметрами при «Продолжить». */
            close_book(a);
            draw_hub(a, 0);
            break;
        case SETTINGS_REDRAW: draw_hub(a, 0); break;
        case SETTINGS_LAUNCH:
            ui_flash(a->g, s, settings_hit(a->settings));
            launch(a, settings_launch_path(a->settings), "Системные настройки");
            break;
        case SETTINGS_QUIT:
            log_msg("хаб: выход");
            pf_quit();
            break;
        case SETTINGS_STOCK_ONCE:
        case SETTINGS_STOCK_FOREVER:
            ui_flash(a->g, s, settings_hit(a->settings));
            stock_home(a, sa == SETTINGS_STOCK_FOREVER);
            break;
        case SETTINGS_NONE: break;
        }
        break;
    }
    }
}

/* Кнопка «Домой»: из книги — в хаб, в хабе — на вкладку «Книги» с начала (как штатный
 * главный экран). Когда хаб — главный экран и активен, прошивка отдаёт её программе. */
static void go_home(app *a)
{
    log_msg("кнопка «Домой»");
    if (a->in_reader && a->reader) {
        to_hub(a, TAB_BOOKS, 1);
        return;
    }
    if (page_busy(a, PENDING_HOME))
        return;
    close_page(a);
    a->info_open = 0;
    a->dlg.open = 0;
    a->quick_open = 0;
    library_close_sheet(a->library);
    while (library_back(a->library))
        ;
    settings_back(a->settings);
    a->tab = TAB_BOOKS;
    draw_hub(a, 0);
}

static void open_quick(app *a)
{
    pf_screen *s = pf_get_screen();
    if (!s)
        return;
    log_msg("быстрые настройки");
    a->quick_open = 1;
    quick_draw(a->qs, s);
}

static void hub_event(app *a, const pf_event *ev)
{
    if (!a->g || !a->home)
        return;
    if (a->quick_open && (ev->type == PF_EVENT_KEY_DOWN || ev->type == PF_EVENT_POINTER_DOWN ||
                          ev->type == PF_EVENT_POINTER_MOVE || ev->type == PF_EVENT_POINTER_UP)) {
        quick_action qa = quick_event(a->qs, ev);
        if (qa != QUICK_NONE) {
            a->quick_open = 0;
            draw_hub(a, qa == QUICK_CLEAN);
        }
        return;
    }
    switch (ev->type) {
    case PF_EVENT_SHOW:
        covers_pause(a->cv, 0);
        draw_hub(a, 1);
        break;
    case PF_EVENT_KEY_DOWN:
        hub_key(a, ev);
        break;
    case PF_EVENT_POINTER_DOWN:
    case PF_EVENT_POINTER_LONG:
    case PF_EVENT_POINTER_MOVE:
    case PF_EVENT_POINTER_UP: {
        gesture g = gesture_feed(&a->gs, ev);
        if ((g.kind == GESTURE_TAP && g.y < UI_STATUS_H) || (g.kind == GESTURE_SWIPE_DOWN && g.y < UI_STATUS_H + 80)) {
            open_quick(a); /* строка состояния — касание или свайп вниз: быстрые настройки */
        } else if (g.kind == GESTURE_TAP) {
            hub_tap(a, &g, ev->time_ms);
        } else if (g.kind == GESTURE_LONG) {
            hub_long(a, &g);
        } else if (g.kind == GESTURE_SWIPE_LEFT || g.kind == GESTURE_SWIPE_RIGHT) {
            /* Свайп по списку — как кнопки листания. */
            pf_event k = *ev;
            k.key = g.kind == GESTURE_SWIPE_LEFT ? PF_KEY_NEXT : PF_KEY_PREV;
            hub_key(a, &k);
        }
        break;
    }
    case PF_EVENT_AUX_TIMER: {
        pf_screen *s = pf_get_screen();
        if (a->quick_open)
            break; /* под шторкой не рисовать (обложки, ход передачи); закроют — draw_hub заведёт таймер снова */
        if (a->page >= 0) {
            page *pg = a->pages[a->page];
            int r = s ? pg->ops->tick(pg, s) : 0;
            if (a->page >= 0 && !(r & (PAGE_CLOSE | PAGE_OPEN)))
                pf_set_aux_timer(PAGE_TICK_MS); /* до перерисовки: draw_hub тоже заводит */
            page_result(a, r, ev->time_ms, 0);
            break;
        }
        if (s && a->info_open) {
            if (bookinfo_cover_ready(a->info, s))
                pf_set_aux_timer(COVER_POLL_MS);
            break;
        }
        if (s && a->tab == TAB_LIBRARY) {
            if (library_covers_ready(a->library, s) > 0)
                pf_set_aux_timer(COVER_POLL_MS);
            break;
        }
        if (!s || a->tab != TAB_BOOKS)
            break;
        int text_changed = 0, waiting = home_covers_ready(a->home, s, &text_changed);
        if (waiting > 0)
            pf_set_aux_timer(COVER_POLL_MS);
        else if (text_changed)
            draw_hub(a, 0); /* авторы из PDF — один раз, когда пришли все обложки */
        break;
    }
    default:
        break;
    }
}

void app_on_event(const pf_event *ev, void *user)
{
    app *a = user;
    switch (ev->type) {
    case PF_EVENT_START:
        on_start(a, ev->time_ms);
        return;
    case PF_EVENT_TIMER:
        /* Таймер только у книги; когда она не на экране — лишь забирает готовое. */
        if (a->reader) {
            reader_on_event(ev, a->reader);
            if (a->in_reader)
                handle_requests(a, ev->time_ms);
        }
        return;
    case PF_EVENT_BACKGROUND:
        save_position(a);
        if (a->cv)
            covers_pause(a->cv, 1);
        if (a->reader)
            reader_on_event(ev, a->reader);
        else
            log_sync();
        return;
    case PF_EVENT_EXIT:
        save_position(a);
        if (a->reader)
            reader_on_event(ev, a->reader);
        log_sync();
        return;
    default:
        break;
    }
    if (ev->type == PF_EVENT_KEY_DOWN && ev->key == PF_KEY_HOME && a->g && a->home) {
        go_home(a);
        return;
    }
    if (a->in_reader && a->reader) {
        reader_on_event(ev, a->reader);
        handle_requests(a, ev->time_ms);
        return;
    }
    hub_event(a, ev);
}
