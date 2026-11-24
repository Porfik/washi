/* positions — на какой странице остановились в каждой книге.
 *
 * Файл <папка данных>/positions.txt, по строке на книгу, последняя открытая — первой:
 *     СТРАНИЦА<TAB>ВСЕГО_СТРАНИЦ<TAB>МАСШТАБ<TAB>ПОЛЯ<TAB>ВИД<TAB>путь
 * Страницы с 1. МАСШТАБ — 0 по странице, 1 по ширине, 2 без полей. ПОЛЯ — «-» или 8 чисел через
 * запятую: рамка содержимого нечётных и чётных страниц в долях страницы (x0,y0,x1,y1).
 * ВИД — «o=ориентация,s=разворот,c=контраст,f=прочитана» (недостающее — 0).
 * Строки без ВИДа и старого вида «СТРАНИЦА<TAB>ВСЕГО<TAB>путь» тоже читаются.
 * Пишется во временный файл и переименовывается, чтобы при сбое не остаться без всего списка.
 */
#ifndef WASHI_POSITIONS_H
#define WASHI_POSITIONS_H

#define POSITIONS_MAX 1000

typedef struct {
    char path[1024];
    int page;     /* с 0 */
    int count;    /* страниц в книге, 0 — неизвестно */
    int view;     /* режим масштаба (view_mode из render.h) */
    int has_crop; /* crop найдены */
    float crop[8];
    int orient;   /* 0 — книжная, 1 и 2 — альбомная (pf_set_orientation) */
    int spread;   /* 1 — две страницы рядом (в альбомной) */
    int contrast; /* 0 — как есть, 1..3 — темнее текст и белее фон */
    int finished; /* дочитана до конца */
} position;

typedef struct {
    position *items; /* items[0] — последняя открытая книга */
    int count;
    char file[1024];
} positions;

/* Нет файла — пустой список (0). -1 — нет памяти. */
int positions_load(positions *p, const char *file);
void positions_free(positions *p);

const position *positions_find(const positions *p, const char *path);

/* Запоминает страницу и поднимает книгу в начало списка (масштаб и поля книги сохраняются). */
void positions_set(positions *p, const char *path, int page, int count);

/* Масштаб и поля книги (книга должна быть в списке, иначе ничего не делает). */
void positions_set_view(positions *p, const char *path, int view, const float *crop);

/* Ориентация, разворот, контраст книги (книга должна быть в списке). */
void positions_set_display(positions *p, const char *path, int orient, int spread, int contrast);

/* Отметка «прочитана» (книга должна быть в списке). */
void positions_set_finished(positions *p, const char *path, int finished);

/* Убрать книгу из списка (из недавних; место чтения забывается). */
void positions_remove(positions *p, const char *path);

/* Книга переименована или перенесена: место чтения, масштаб и поля остаются за новым путём (место в
 * списке прежнее). Старая запись по новому пути, если была, заменяется. */
void positions_rename(positions *p, const char *from, const char *to);

/* То же для папки: все книги под from/ получают путь под to/. */
void positions_rename_prefix(positions *p, const char *from, const char *to);

/* 0 — записано. */
int positions_save(const positions *p);

#endif
