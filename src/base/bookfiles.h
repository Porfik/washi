/* bookfiles — переименовать и удалить книгу или папку в библиотеке (library_root).
 *
 * Всё только внутри корня библиотеки: сам корень, папки прошивки и хаба в нём (system, applications,
 * washi, …), «..» и пути вне корня отвергаются; символические ссылки не разворачиваются (удаляется
 * ссылка, а не то, на что она указывает). Имена проходят net_safe_name (FAT32).
 * Вместе с файлом переезжают (или убираются) место чтения (positions), закладки (bookmarks_file)
 * и обложка (covers; NULL — без обложек, например в тесте). positions сохраняется на диск.
 * Открытую книгу вызывающий закрывает сам до вызова.
 */
#ifndef WASHI_BASE_BOOKFILES_H
#define WASHI_BASE_BOOKFILES_H

#include <stddef.h>

#include "../doc/covers.h"
#include "positions.h"

typedef enum {
    BF_OK = 0,
    BF_SAME,    /* имя не изменилось — ничего не сделано */
    BF_EXISTS,  /* такое имя уже занято */
    BF_INVALID, /* пустое имя */
    BF_OUTSIDE, /* не в библиотеке или трогать нельзя */
    BF_FAILED,  /* файловая система отказала */
} bf_result;

/* Человеческий текст ошибки (для окна). */
const char *bookfiles_message(bf_result r);

/* Имя книги без расширения: «Война и мир» из «…/Война и мир.pdf» (расширение — .fb2.zip учитывается). */
void bookfiles_stem(const char *path, char *out, size_t len);

/* Книга path получает имя stem + прежнее расширение (stem — как ввёл человек; расширение, если он его
 * дописал, не удваивается). В newpath — новый путь (и при BF_SAME — прежний). */
bf_result bookfiles_rename_book(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                                const char *path, const char *stem, char *newpath, size_t len);

bf_result bookfiles_delete_book(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                                const char *path);

/* Папка dir получает имя name (в той же папке); все книги в ней переезжают. */
bf_result bookfiles_rename_dir(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                               const char *dir, const char *name, char *newdir, size_t len);

/* Папка со всем содержимым. */
bf_result bookfiles_delete_dir(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                               const char *dir);

#endif
