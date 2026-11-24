/* bookmarks — закладки книг: файл <папка данных>/bookmarks.txt, по строке на книгу:
 *     СТР,СТР,…<TAB>путь       (страницы с 1, по возрастанию)
 * Пишется во временный файл и переименовывается. */
#ifndef WASHI_BASE_BOOKMARKS_H
#define WASHI_BASE_BOOKMARKS_H

#define BOOKMARKS_MAX 512 /* закладок в одной книге */

typedef struct {
    char file[1024];
    char path[1024]; /* книга */
    int pages[BOOKMARKS_MAX]; /* с 0, по возрастанию */
    int count;
} bookmarks;

/* Закладки книги path из file (нет файла или книги — пусто). */
void bookmarks_load(bookmarks *b, const char *file, const char *path);

int bookmarks_has(const bookmarks *b, int page);

/* Поставить или снять; возвращает 1 — теперь стоит, 0 — снята. Сразу записывает файл. */
int bookmarks_toggle(bookmarks *b, int page);

/* Книга (или, при folder, все книги папки from/…) переименована: закладки переезжают на новый путь;
 * to == NULL — книги удалены, закладки убираются. Переписывает файл; 0 — записан (или менять нечего). */
int bookmarks_move(const char *file, const char *from, const char *to, int folder);

#endif
