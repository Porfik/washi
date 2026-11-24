/* webdav — разбор ответа PROPFIND (multistatus). Сам запрос — remote_list (remote.c). */
#ifndef WASHI_NET_WEBDAV_H
#define WASHI_NET_WEBDAV_H

#include <stddef.h>

#include "remote.h"

/* Тело запроса PROPFIND (Depth: 1): тип, размер, время изменения. */
extern const char webdav_propfind_body[];

/* Ответ multistatus xml (len байт) для папки с путём dir_path в адресе (раскодированным,
 * без «/» в конце: «/Книги/Фантастика», у корня сервера — ""). Элементы — прямые дети папки.
 * Пространства имён (d:, D:, lp1:, без префикса), href с %XX и &amp;, абсолютные и
 * относительные href. Если ни один href не лёг под dir_path (сервер отдаёт пути иначе, например
 * за прокси), первым считается сама папка, остальные — её дети по последней части пути.
 * Возвращает число элементов; -1 — это не multistatus. */
int webdav_parse(const char *xml, size_t len, const char *dir_path, remote_entries *out);

/* Дата HTTP («Tue, 03 Nov 2026 10:00:00 GMT») или ISO 8601 → секунды UTC; 0 — не разобрать. */
long long webdav_parse_date(const char *s);

#endif
