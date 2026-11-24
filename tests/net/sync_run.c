/* Синхронизация без интерфейса — для проверки против настоящего сервера (rclone serve webdav / ftp):
 *   sync_run АДРЕС ЛОГИН ПАРОЛЬ ПАПКА_КНИГ ПАПКА_ДАННЫХ [cancel_after_ms]
 * Делает то же, что страница хаба: задача, слияние мест чтения в «потоке интерфейса»
 * (positions.txt и bookmarks.txt в ПАПКЕ_ДАННЫХ), печатает итог. */
#include "../../src/net/syncjob.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *data_dir = ".";

const char *pf_data_dir(void)
{
    return data_dir;
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "sync_run АДРЕС ЛОГИН ПАРОЛЬ ПАПКА_КНИГ ПАПКА_ДАННЫХ [отмена_через_мс]\n");
        return 2;
    }
    data_dir = argv[5];
    sync_job_cfg c;
    memset(&c, 0, sizeof c);
    char err[160];
    if (remote_normalize(argv[1], c.remote.url, sizeof c.remote.url, err, sizeof err) != 0) {
        printf("адрес: %s\n", err);
        return 1;
    }
    snprintf(c.remote.login, sizeof c.remote.login, "%s", argv[2]);
    snprintf(c.remote.password, sizeof c.remote.password, "%s", argv[3]);
    snprintf(c.local_dir, sizeof c.local_dir, "%s", argv[4]);
    snprintf(c.data_dir, sizeof c.data_dir, "%s", argv[5]);
    int cancel_ms = argc > 6 ? atoi(argv[6]) : 0;

    char pf[600];
    snprintf(pf, sizeof pf, "%s/positions.txt", argv[5]);
    positions pos;
    positions_load(&pos, pf);

    sync_job *j = sync_job_start(&c);
    if (!j)
        return 1;
    int merged = 0, waited = 0;
    notes_result nr;
    memset(&nr, 0, sizeof nr);
    sync_status st;
    for (;;) {
        sync_job_status(j, &st);
        if (st.stage == SJ_MERGE && !merged) {
            merged = 1;
            int rc = notes_merge(&pos, sync_job_files(j), &nr);
            sync_job_merged(j, rc, &nr);
        }
        if (st.stage == SJ_DONE)
            break;
        nanosleep(&(struct timespec){ 0, 20000000 }, NULL);
        waited += 20;
        if (cancel_ms && waited >= cancel_ms && waited < cancel_ms + 20)
            sync_job_cancel(j);
    }
    sync_job_free(j);
    printf("итог: новых %d, обновлено %d, не скачано %d (%s), места %d, конфликтов %d, на сервере удалено %d, "
           "нет места %d, папок не открылось %d, всего на сервере %d\n",
           st.n_new, st.n_updated, st.n_failed, st.first_fail, st.todo, st.n_conflict, st.n_server_deleted,
           st.n_nospace, st.n_dirs_failed, st.n_total_books);
    printf("слияние: мест обновлено %d, добавлено %d, закладок %d, отправить %d/%d, отправлено файлов %d\n",
           nr.pos_updated, nr.pos_imported, nr.bm_updated, nr.send_pos, nr.send_bm, st.sent_notes);
    printf("ошибка: «%s», заметки: «%s», отменено: %d\n", st.error, st.notes_error, st.cancelled);
    positions_free(&pos);
    return st.error[0] ? 1 : 0;
}
