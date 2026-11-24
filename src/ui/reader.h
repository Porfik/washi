/* Экран чтения: страница по выбранному масштабу, листание кнопками, касаниями и свайпами,
 * панель (касание вверху, долгое касание, кнопка меню), содержание. Жесты — docs/ux.md.
 * Страницы рисуют фоновые потоки (render), готовые лежат в кэше (cache);
 * поток интерфейса только копирует готовую страницу в буфер экрана. */
#ifndef WASHI_UI_READER_H
#define WASHI_UI_READER_H

#include "../doc/doc.h"
#include "../platform/platform.h"

typedef struct {
    const char *path;       /* NULL — файл не указан */
    int start_page;         /* с какой страницы открыть (с 0) */
    int view;               /* масштаб: view_mode из render.h */
    int has_crop;           /* найдены поля книги (для «без полей») */
    float crop[8];          /* рамки содержимого нечётных и чётных страниц, доли страницы */
    int orient;             /* ориентация экрана книги (pf_set_orientation): 0 — книжная */
    int spread;             /* 1 — разворот из двух страниц (только в альбомной) */
    int contrast;           /* 0..3 — контраст (темнее текст, белее фон) */
    int full_refresh_every; /* полное обновление раз в N перелистываний; 0 — никогда */
    size_t cache_bytes;     /* лимит кэша готовых страниц */
    int prefetch_ahead;     /* сколько страниц вперёд рисовать заранее */
    int prefetch_behind;    /* и сколько назад */
    int render_threads;     /* потоков отрисовки (по копии документа на поток) */
    int prefetch_while_waiting; /* 1 — пока ждём нужную страницу, рисовать и соседние */
    int screen_busy_ms;     /* сколько экран занят после обновления; нажатия за это время
                               не показываются по одному, а сдвигают страницу; 0 — не ждать */
    int show_hint;          /* 1 — при первом показе страницы показать подсказку по жестам */
    int reflow_em;          /* кегль «перетекающих» книг (EPUB/FB2/HTML) в пунктах; 0 — 28 */
    doc_options doc;
} reader_options;

typedef struct reader reader;

reader *reader_new(const reader_options *opt);
void reader_free(reader *r);

/* Обработчик событий платформы. Открытый не при запуске программы получает
 * PF_EVENT_START, затем PF_EVENT_SHOW. */
void reader_on_event(const pf_event *ev, void *user);

const char *reader_path(const reader *r);

/* Страница, к которой пришли (с 0), и всего страниц; 0 страниц — документ не открыт. */
int reader_page(const reader *r);
int reader_page_count(const reader *r);

/* Текущие настройки (масштаб, поля, перерисовка, параметры документа). */
const reader_options *reader_current_options(const reader *r);

/* Чего читалка просит у программы (биты сбрасываются при чтении). */
enum {
    READER_WANT_LIBRARY = 1,   /* библиотека (меню или любая кнопка на экране ошибки) */
    READER_WANT_REOPEN = 4,    /* изменились параметры документа или ориентация: открыть книгу заново на той же
                                  странице (ориентацию ставит программа — reader_current_options()->orient) */
    READER_SAVE_BOOK = 8,      /* изменились масштаб, поля, ориентация, разворот или контраст книги */
    READER_HINT_SHOWN = 32,    /* подсказка по жестам показана: больше не показывать */
    READER_FINISHED = 64,      /* дочитали до последней страницы */
};
int reader_take_requests(reader *r);

/* Книга уходит с экрана: дорисованное не показывать, отрисовку заранее отменить. */
void reader_pause(reader *r);

/* Снова на экране: показать страницу полным обновлением. */
void reader_resume(reader *r, double t);

#endif
