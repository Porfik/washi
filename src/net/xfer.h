/* xfer — общее у серверов приёма (httpd.c, ftpd.c) и экрана «Приём по Wi‑Fi»: журнал приёма
 * (идущие и принятые файлы), сокеты, место на диске, перенос готового файла на своё место.
 *
 * Серверы пишут в журнал из своих потоков, экран читает снимком (xfer_snapshot) в tick.
 * Все функции можно звать из любого потока.
 */
#ifndef WASHI_NET_XFER_H
#define WASHI_NET_XFER_H

#include <pthread.h>
#include <stddef.h>

#define XFER_DONE_MAX 48   /* сколько принятых помнить (новые вытесняют старые) */
#define XFER_ACTIVE_MAX 8  /* одновременно идущих приёмов */

typedef struct {
    char name[256]; /* как будет называться на устройстве (для идущих — как просили) */
    long long size; /* принято (для идущих — сколько уже) */
    long long total; /* для идущих: сколько всего; -1 — неизвестно */
    int via;        /* 0 — браузер, 1 — FTP */
    int id;
} xfer_item;

typedef struct {
    pthread_mutex_t mu;
    xfer_item done[XFER_DONE_MAX]; /* новые — первыми */
    int ndone;
    xfer_item active[XFER_ACTIVE_MAX];
    int nactive;
    int next_id;
    int gen;       /* растёт при любом изменении списков (начало, конец, ход) */
    int books_gen; /* растёт, когда на устройстве появился новый файл */
    int total_done; /* сколько принято за всё время */
    double last_activity_ms; /* clock_ms() последнего события: соединение, команда, байты */
    char pass[256];  /* пароль приёма (открытый, только в памяти); пусто — без пароля */
    int pass_gen;    /* растёт при смене пароля */
    int allow_edit;  /* из браузера можно удалять и переименовывать (включает владелец на устройстве) */
} xfer;

void xfer_init(xfer *x);
void xfer_destroy(xfer *x);

/* Что-то произошло (соединение, команда, данные) — сдвинуть отсчёт «без передач». */
void xfer_touch(xfer *x);

/* Начало приёма. Возвращает номер для progress/end или -1, если слишком много приёмов сразу.
 * total — по Content-Length, -1 — неизвестно. */
int xfer_begin(xfer *x, const char *name, long long total, int via);
void xfer_progress(xfer *x, int id, long long done);
/* Конец. ok — файл на месте (под именем final_name, size байт): попадает в список принятых;
 * иначе просто убирается из идущих. */
void xfer_end(xfer *x, int id, int ok, const char *final_name, long long size);

typedef struct {
    xfer_item done[XFER_DONE_MAX];
    int ndone;
    xfer_item active[XFER_ACTIVE_MAX];
    int nactive;
    int gen, books_gen, total_done;
    double last_activity_ms;
} xfer_snap;
void xfer_snapshot(xfer *x, xfer_snap *out);

/* Сколько приёмов идёт сейчас. */
int xfer_active_count(xfer *x);

/* ---- пароль приёма (общий у HTTP и FTP; смена действует сразу, под мьютексом) ---- */

/* Задать пароль; NULL или пусто — приём без пароля. Длиннее 255 байт — обрезается. */
void xfer_set_password(xfer *x, const char *plain);
int xfer_has_password(xfer *x);
int xfer_password_gen(xfer *x);
/* Подходит ли given (NULL — ничего не ввели). Пароля нет — всегда 1. Сравнение за постоянное время. */
int xfer_check_password(xfer *x, const char *given);
/* Пауза ~1 с после неверной попытки (от перебора); раньше обрывается, если *stop. */
void xfer_auth_pause(volatile int *stop);

/* Файл пароля (<папка данных>/recv.txt): password=<зашифрованный>. load: 1 — пароль есть и
 * расшифровался (out — открытый), 0 — нет файла/пароля, -1 — пароль был, но не расшифровался
 * (другое устройство, порча): out — случайный пароль, которого никто не знает (приём закрыт,
 * пока не зададут новый или не уберут). save: пустой plain — убрать пароль (файл удаляется). 0 — готово. */
int xfer_pass_load(const char *file, char *out, size_t len);
int xfer_pass_save(const char *file, const char *plain);

/* ---- разрешение «удалять и переименовывать из браузера» (по умолчанию выключено; смена действует сразу) ---- */

void xfer_set_allow_edit(xfer *x, int on);
int xfer_allow_edit(xfer *x);
/* Файлы на устройстве изменились не приёмом (удалили, переименовали): библиотеке перечитаться. */
void xfer_books_changed(xfer *x);
/* Хранится в том же recv.txt: edit=1. load: 1 — разрешено, 0 — нет (или нет файла).
 * save не трогает пароль в файле; файл удаляется, когда в нём нечего хранить. 0 — готово. */
int xfer_edit_load(const char *file);
int xfer_edit_save(const char *file, int on);

/* ---- сокеты ---- */

/* Слушающий сокет на 0.0.0.0: порт first, занят — следующий (до tries штук); first == 0 —
 * любой свободный. Возвращает fd или -1; *port — занятый порт. */
int xfer_listen(int first, int tries, int *port);

/* Ждать, пока fd станет готов (POLLIN или POLLOUT, events) не дольше ms; каждые ~200 мс
 * смотрит *stop. 1 — готов, 0 — время вышло, -1 — остановка или ошибка. */
int xfer_wait(int fd, int events, int ms, volatile int *stop);

/* Отправить всё (MSG_NOSIGNAL, ждёт не дольше 30 с без движения). 0 — отправлено, -1 — нет. */
int xfer_send_all(int fd, const void *buf, size_t len, volatile int *stop);

/* ---- файлы ---- */

/* Свободно на диске, где лежит (или будет лежать) path, байт; -1 — не узнать. */
long long xfer_free_space(const char *path);

/* Временный файл рядом с будущим: «dir/.recv-N.part» (имя уникально для процесса).
 * Возвращает fd (запись) или -1. */
int xfer_tmp_open(const char *dir, char *path, size_t len);

/* Убрать забытые «.recv-*.part» из dir (после прошлого сбоя). */
void xfer_tmp_clean(const char *dir);

/* Поставить готовый временный файл на место: в папке dir под именем name (net_safe_name).
 * Занято: unique != 0 — берётся «name (2).ext» и т. д., иначе ошибка (errno = EEXIST).
 * Итоговый путь — в out. 0 — готово, -1 — нет (временный файл остаётся). */
int xfer_commit(const char *tmp, const char *dir, const char *name, int unique, char *out, size_t len);

#endif
