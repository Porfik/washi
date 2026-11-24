/* bookinfo — карточка книги (долгое касание книги в хабе), как в Литрес и Яндекс Книгах:
 * большая обложка, название, автор, цифры (страниц, прочитано, размер, где остановились),
 * «Продолжить чтение», «С начала», «Переименовать», «Удалить», «Убрать из недавних». */
#ifndef WASHI_UI_BOOKINFO_H
#define WASHI_UI_BOOKINFO_H

#include "../base/positions.h"
#include "../doc/covers.h"
#include "gfx.h"
#include "widgets.h"

typedef struct bookinfo bookinfo;

typedef enum {
    BOOKINFO_NONE,
    BOOKINFO_BACK,   /* закрыть карточку */
    BOOKINFO_READ,   /* открыть с места, где остановились */
    BOOKINFO_START,  /* открыть с первой страницы */
    BOOKINFO_REMOVE, /* убрать из недавних */
    BOOKINFO_RENAME, /* спросить новое имя файла */
    BOOKINFO_DELETE, /* спросить, удалять ли файл */
} bookinfo_action;

bookinfo *bookinfo_new(gfx *g, covers *cv, const positions *pos, const char *root);
void bookinfo_free(bookinfo *b);

void bookinfo_open(bookinfo *b, const char *path);
const char *bookinfo_path(const bookinfo *b);

/* Содержимое между строкой состояния и вкладками. */
void bookinfo_draw(bookinfo *b, pf_screen *s);
/* Обложка была не готова — дорисовать и обновить; 1 — ещё ждём. */
int bookinfo_cover_ready(bookinfo *b, pf_screen *s);

bookinfo_action bookinfo_tap(bookinfo *b, int x, int y, ui_rect *hit);

#endif
