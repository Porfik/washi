/* syncnotes — слияние мест чтения (positions.txt) и закладок (bookmarks.txt) книги и сервера.
 *
 * На сервере в папке .washi/ лежат файлы того же вида, что и на книге, но вместо полного
 * пути книги в них «ключ»: для книг из папки синхронизации — путь внутри неё («Серия/книга.epub»,
 * с таким ключом место чтения переносится между устройствами), для остальных — полный путь
 * (резервная копия этого устройства).
 *
 * Слияние трёхстороннее: к «книге» и «серверу» добавлена «база» — то, что сошлось в прошлый раз
 * (файл sync-notes.txt в папке данных). Кто изменился относительно базы — тот и прав; изменились
 * оба (или базы ещё нет) — дальняя страница. Закладки: объединение, но снятая с одной стороны
 * относительно базы закладка снимается и с другой. Меняется только то, что относится к книгам,
 * которые есть на этом устройстве; остальное на сервере остаётся как было.
 *
 * Вызывается из потока интерфейса (меняет positions хаба и bookmarks.txt).
 */
#ifndef WASHI_NET_SYNCNOTES_H
#define WASHI_NET_SYNCNOTES_H

#include <stddef.h>

#include "../base/positions.h"

typedef struct {
    const char *sync_dir;  /* папка синхронизации на книге (без «/» в конце) */
    const char *srv_pos;   /* копия positions.txt с сервера (может не существовать) */
    const char *srv_bm;    /* копия bookmarks.txt с сервера */
    const char *loc_bm;    /* bookmarks.txt на книге */
    const char *base;      /* sync-notes.txt (может не существовать) */
    const char *up_pos;    /* куда записать слитое для отправки */
    const char *up_bm;
    const char *new_base;  /* куда записать новую базу (в базу её переименуют после отправки) */
} notes_files;

typedef struct {
    int pos_updated;  /* мест чтения на книге изменено */
    int pos_imported; /* добавлено с сервера (книга на устройстве есть, места не было) */
    int bm_updated;   /* книг, у которых на книге изменились закладки */
    int send_pos;     /* слитое отличается от серверного — отправить */
    int send_bm;
} notes_result;

/* Ключ книги по пути на книге и обратно. */
void notes_key(const char *sync_dir, const char *path, char *key, size_t len);
void notes_path(const char *sync_dir, const char *key, char *path, size_t len);

/* Слить. 0 — готово (положено в notes_result и в файлы up_*, new_base); -1 — не хватило памяти
 * или не записать файлы. local сохраняется (positions_save), если изменился. */
int notes_merge(positions *local, const notes_files *f, notes_result *r);

#endif
