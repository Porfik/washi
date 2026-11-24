/* remote — «удалённая папка» на сервере: WebDAV (http://, https://) или FTP (ftp://).
 * Список папки, скачать и отправить файл — одинаково для обоих; всё через net_perform,
 * то есть блокирует и зовётся из своего потока.
 *
 * Пути внутри папки — относительные, UTF-8, через «/», без «/» в начале и конце.
 */
#ifndef WASHI_NET_REMOTE_H
#define WASHI_NET_REMOTE_H

#include <stddef.h>

typedef struct {
    char url[1024]; /* адрес папки, как remote_normalize его выдал (без «/» в конце) */
    char login[128];
    char password[128];
} remote_cfg;

typedef struct {
    char name[256]; /* имя одного элемента (без пути) */
    int is_dir;
    long long size;  /* -1 — неизвестен */
    long long mtime; /* секунды UTC, 0 — неизвестно */
} remote_entry;

typedef struct {
    remote_entry *items;
    int count, cap;
} remote_entries;

void remote_entries_free(remote_entries *e);
int remote_entries_add(remote_entries *e, const char *name, int is_dir, long long size, long long mtime);

/* Что ввёл человек → годный адрес: без схемы — https://, пробелы по краям убраны, путь
 * закодирован (русские буквы → %XX, готовые %XX остаются), «/» в конце убран.
 * 0 — адрес годен; -1 — нет (err — почему, по-русски). */
int remote_normalize(const char *in, char *out, size_t len, char *err, size_t err_len);

int remote_is_ftp(const char *url);

/* Адрес без схемы и пароля для показа: «webdav.yandex.ru/Книги». */
void remote_display(const char *url, char *out, size_t len);

/* Содержимое папки dir ("" — корень). Заполняет out (освобождать remote_entries_free).
 * 0 — есть; -1 — нет (err — по-русски, с подсказкой; status — код ответа или 0). */
int remote_list(const remote_cfg *c, const char *dir, remote_entries *out, volatile int *cancel, char *err,
                size_t err_len, long *status);

/* Скачать файл rel в local (через local.part). progress — ход, как в net_request. */
int remote_get(const remote_cfg *c, const char *rel, const char *local, volatile int *cancel,
               void (*progress)(void *user, long long done, long long total), void *user, char *err,
               size_t err_len, long *status);

/* Отправить файл local как rel; недостающие папки на сервере создаются. */
int remote_put(const remote_cfg *c, const char *rel, const char *local, volatile int *cancel, char *err,
               size_t err_len);

/* Проба: открыть корневую папку. 0 — всё хорошо, files и dirs — сколько в ней. */
int remote_check(const remote_cfg *c, int *files, int *dirs, char *err, size_t err_len);

/* Разбор ответов FTP-сервера (тексты — для проверки и для remote_list). MLSD:
 * «type=file;size=12;modify=20261124103000; имя». LIST — как у unix (ls -l) или IIS (DOS).
 * Возвращают число найденных элементов; «.» и «..» пропускаются. */
int remote_parse_mlsd(const char *text, remote_entries *out);
int remote_parse_list(const char *text, remote_entries *out);

#endif
