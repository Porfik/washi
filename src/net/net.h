/* net — общее для сетевых экранов хаба: адрес книги в сети, запросы HTTP(S) и FTP через
 * libcurl, имена файлов для принятых книг.
 *
 * libcurl берётся через dlopen (libcurl.so.4): на книге — из прошивки, с её OpenSSL
 * (TLS 1.2) и сертификатами, на ПК — системная. Если библиотеки нет, сетевые экраны говорят
 * об этом, остальной хаб работает как прежде. Свои корневые сертификаты, если прошивкины
 * не подойдут, — файл <папка данных>/cacert.pem.
 *
 * net_perform блокирует: звать из своего потока, не из потока интерфейса. Остальное — из любого.
 */
#ifndef WASHI_NET_NET_H
#define WASHI_NET_NET_H

#include <stddef.h>

/* IPv4-адрес книги в локальной сети («192.168.1.23»). 0 — есть, -1 — сети нет. */
int net_local_ip(char *buf, size_t len);

/* Есть ли libcurl. 0 — есть; -1 — нет, why — почему (по-русски, для показа). */
int net_http_ready(char *why, size_t len);

/* Поддерживает ли libcurl протокол ("ftp", "ftps", "https"…). */
int net_has_protocol(const char *proto);

typedef struct {
    const char *url;
    const char *method;          /* NULL — GET (PUT, если upload_path); "PROPFIND", "MKCOL"… */
    const char *login, *password; /* вход (Basic или Digest — какой попросит сервер); NULL — без */
    const char *const *headers;  /* свои заголовки «Имя: значение», список до NULL; или NULL */
    const char *body;            /* тело запроса (PROPFIND, POST); NULL — без */
    size_t body_len;
    const char *upload_path;     /* отправить этот файл (PUT; для ftp:// — STOR) */
    int create_dirs;             /* FTP: создать недостающие папки при отправке */
    const char *out_path;        /* ответ — в файл: пишется в «out_path.part», при успехе
                                    переименовывается в out_path; NULL — ответ в память */
    size_t max_body;             /* ответ в памяти не больше (0 — 8 МБ) */
    int timeout_s;               /* предел на весь запрос; 0 — без него (но 60 с без данных — обрыв) */
    volatile int *cancel;        /* != 0 — прервать как можно скорее (ошибка «Отменено») */
    /* Ход передачи (из потока запроса): сколько принято или отправлено, total -1 — неизвестно. */
    void (*progress)(void *user, long long done, long long total);
    void *user;
} net_request;

typedef struct {
    long status;           /* код ответа HTTP или FTP (0 — ответа не было) */
    char *body;            /* ответ в памяти, с нулём в конце; NULL при out_path */
    size_t len;
    char content_type[128];
    char filename[256];    /* имя из Content-Disposition (если сервер дал), иначе "" */
    char final_url[2048];  /* адрес после переадресаций */
    char error[256];       /* что не так, по-русски, для показа; "" — всё хорошо */
} net_response;

/* 0 — ответ получен, код 2xx (для FTP — любой успешный); -1 — нет (rs->error, rs->status).
 * rs заполняется всегда, освобождать — net_response_free. */
int net_perform(const net_request *rq, net_response *rs);
void net_response_free(net_response *rs);

/* Ссылка href относительно base (RFC 3986: схема, «//хост», «/путь», «?запрос», «../»). */
void net_url_resolve(const char *base, const char *href, char *out, size_t len);

/* Процентное кодирование для части пути URL: всё, кроме A-Z a-z 0-9 - . _ ~ и (если
 * keep_slash) «/». Русские имена файлов — байтами UTF-8 (%D0%9A…). */
void net_url_encode(const char *s, int keep_slash, char *out, size_t len);

/* Обратное: %XX → байт ('+' не трогается). */
void net_url_decode(const char *s, char *out, size_t len);

/* Имя файла, годное для FAT32 (берётся последняя часть после / или \): : * ? " < > | и
 * управляющие → «_», без пробелов и точек
 * в начале и в конце, не длиннее 200 байт (UTF-8 не режется посередине, расширение
 * сохраняется). Пустое — "book". */
void net_safe_name(const char *name, char *out, size_t len);

/* Свободный путь для файла name в папке dir: «dir/name», занят — «dir/name (2).ext» и т. д.
 * Папка dir создаётся (со всеми родителями). name проходит net_safe_name. 0 — путь есть. */
int net_unique_path(const char *dir, const char *name, char *out, size_t len);

/* Создать папку со всеми родителями. 0 — есть. */
int net_mkdirs(const char *dir);

#endif
