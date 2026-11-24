/* httpd — маленький HTTP-сервер приёма книг: страница для браузера, загрузка файла сырым телом
 * (PUT или POST /upload?name=…), список принятых (GET /list). Свой поток на приём соединений и
 * по потоку на каждое (до HTTPD_MAX_CONN). Принятое пишется потоком во временный файл в
 * папке приёма и переносится на место после последнего байта; обрыв — файл удаляется.
 * Задан пароль (xfer_set_password) — всё, включая страницу, за Basic-авторизацией (логин любой).
 *
 * Папки (пути — относительно корня библиотеки, UTF-8, %XX):
 *   GET  /ls[?dir=]               папки и файлы (с размером) → {"dir","free","edit","def","dirs":[{name}],"files":[{name,size}]}
 *   GET  /count?path=             сколько файлов в папке (для вопроса перед удалением)
 *   POST /mkdir?dir=&name=        новая папка (всегда можно)
 *   POST /delete?path=            удалить файл или папку — только при xfer_allow_edit, иначе 403
 *   POST /rename?path=&name=      переименовать — то же; имя занято — 409
 *   PUT  /upload?name=[&dir=]     в выбранную папку (без dir — папка приёма)
 * Изменяющие запросы — только с заголовком «X-Requested-With: washi» (его ставит страница; чужая
 * страница в браузере без разрешения такой заголовок послать не может).
 */
#ifndef WASHI_NET_HTTPD_H
#define WASHI_NET_HTTPD_H

#include "xfer.h"

#define HTTPD_PORT 8080
#define HTTPD_MAX_CONN 6

typedef struct httpd httpd;

/* Запустить. dir — папка приёма (куда класть книги без dir=; создаётся); root — корень библиотеки
 * (library_root): внутри него страница показывает папки, принимает в выбранную, создаёт папки,
 * а с разрешения (xfer_set_allow_edit) удаляет и переименовывает; NULL — только папка приёма.
 * hidden_dir — ещё одна закрытая папка (данные хаба; может быть NULL); system и applications в
 * корне закрыты всегда. Порт first, занят — следующий. NULL — не вышло. */
httpd *httpd_start(xfer *log, const char *dir, const char *root, const char *hidden_dir, int first_port);
int httpd_port(const httpd *h);
/* Остановить: приёмы обрываются, недописанное удаляется. Возвращается, когда потоки кончились. */
void httpd_stop(httpd *h);

#endif
