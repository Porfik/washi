/* syncjob — одна синхронизация с папкой на сервере, в своём потоке:
 *   1. обойти папки сервера, скачать новые и изменившиеся книги в локальную папку
 *      (локальное не удаляется и изменённое на книге не перезаписывается);
 *   2. скачать с сервера копии мест чтения и закладок (.washi/positions.txt, bookmarks.txt);
 *   3. остановиться и ждать слияния — его делает поток интерфейса (notes_merge, syncnotes.h),
 *      потому что меняет данные хаба; потом sync_job_merged;
 *   4. отправить слитое обратно.
 * Состояние (что скачано) — sync-books.txt в папке данных; база слияния — sync-notes.txt.
 */
#ifndef WASHI_NET_SYNCJOB_H
#define WASHI_NET_SYNCJOB_H

#include "remote.h"
#include "syncnotes.h"

typedef struct {
    remote_cfg remote;
    char local_dir[1024]; /* куда класть книги (папка синхронизации на книге) */
    char data_dir[512];   /* папка хаба: состояние и временные файлы */
} sync_job_cfg;

enum { SJ_LIST, SJ_BOOKS, SJ_NOTES, SJ_MERGE, SJ_UPLOAD, SJ_DONE };

typedef struct {
    int stage;
    int cancelled;
    char error[256];       /* SJ_DONE: синхронизация не удалась (по-русски, с подсказкой) */
    char notes_error[256]; /* книги скачаны, а места чтения и закладки — нет */
    int dirs, files;       /* найдено на сервере: папок, книг */
    int todo, done;        /* книг к скачиванию и обработано */
    long long bytes_total, bytes_done;
    char cur[256];         /* какая книга сейчас */
    long long cur_done, cur_total;
    int n_new, n_updated;
    int n_failed;          /* не скачались */
    char first_fail[256];  /* почему (первая) */
    int n_nospace;         /* не хватило места на книге */
    int n_conflict;        /* на книге файл изменён или другой — не тронут */
    int n_server_deleted;  /* были на сервере, пропали (на книге остались) */
    int n_dirs_failed;     /* папок на сервере не открылись */
    int n_total_books;     /* всего книг на сервере */
    int sent_notes;        /* на сервер отправлено файлов (мест чтения, закладок) */
} sync_status;

typedef struct sync_job sync_job;

/* NULL — не хватило памяти. */
sync_job *sync_job_start(const sync_job_cfg *c);
void sync_job_status(sync_job *j, sync_status *out);
void sync_job_cancel(sync_job *j);

/* Файлы для notes_merge (valid, пока жива работа). sync_dir — папка книг. */
const notes_files *sync_job_files(const sync_job *j);

/* После notes_merge (ok == 0) — отправить слитое; ok != 0 — слияние не вышло, отправлять нечего. */
void sync_job_merged(sync_job *j, int ok, const notes_result *r);

/* Прервать и дождаться потока. */
void sync_job_free(sync_job *j);

/* Книга ли файл по имени (расширение). */
int sync_is_book(const char *name);

#endif
