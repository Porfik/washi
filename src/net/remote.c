/* _DEFAULT_SOURCE: strcasecmp, strncasecmp, timegm. */
#define _DEFAULT_SOURCE
#include "remote.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "net.h"
#include "webdav.h"

/* ---- Список элементов ---- */

void remote_entries_free(remote_entries *e)
{
    free(e->items);
    memset(e, 0, sizeof *e);
}

int remote_entries_add(remote_entries *e, const char *name, int is_dir, long long size, long long mtime)
{
    if (e->count == e->cap) {
        int cap = e->cap ? e->cap * 2 : 64;
        remote_entry *v = realloc(e->items, sizeof *v * (size_t)cap);
        if (!v)
            return -1;
        e->items = v;
        e->cap = cap;
    }
    remote_entry *it = &e->items[e->count++];
    snprintf(it->name, sizeof it->name, "%s", name);
    it->is_dir = is_dir;
    it->size = size;
    it->mtime = mtime;
    return 0;
}

/* ---- Адрес ---- */

int remote_is_ftp(const char *url)
{
    return !strncasecmp(url, "ftp://", 6);
}

/* Имя сервера (с логином и портом) годно: без пробелов и лишних знаков, порт — число. */
static int check_host(const char *host, size_t hl, char *err, size_t err_len)
{
    const char *name = host;
    for (size_t i = 0; i < hl; i++)
        if (host[i] == '@')
            name = host + i + 1; /* логин:пароль@ — до последней «@» */
    size_t nl = (size_t)(host + hl - name);
    const char *colon = NULL;
    int alnum = 0;
    for (size_t i = 0; i < nl; i++) {
        unsigned char ch = (unsigned char)name[i];
        if (ch == ':' && !colon) {
            colon = name + i;
            continue;
        }
        if (isspace(ch) || ch < 0x20 || ch == 0x7f || strchr("<>\"\\^`{|}#?%", ch)) {
            if (isspace(ch) || ch < 0x20)
                snprintf(err, err_len, "В имени сервера не должно быть пробелов — проверьте адрес");
            else
                snprintf(err, err_len, "В имени сервера недопустим знак «%c» — проверьте адрес", (char)ch);
            return -1;
        }
        if (!colon && (isalnum(ch) || ch >= 0x80))
            alnum = 1;
        if (colon && !isdigit(ch)) {
            if (name[0] == '[')
                continue; /* IPv6: «[::1]:8080» */
            snprintf(err, err_len, "Порт в адресе должен быть числом");
            return -1;
        }
    }
    if (!alnum && name[0] != '[') {
        snprintf(err, err_len, "В адресе нет имени сервера");
        return -1;
    }
    return 0;
}

int remote_normalize(const char *in, char *out, size_t len, char *err, size_t err_len)
{
    char buf[1024];
    while (isspace((unsigned char)*in))
        in++;
    snprintf(buf, sizeof buf, "%s", in);
    size_t n = strlen(buf);
    while (n && isspace((unsigned char)buf[n - 1]))
        buf[--n] = 0;
    if (!n) {
        snprintf(err, err_len, "Введите адрес папки на сервере");
        return -1;
    }
    char full[1100];
    if (strstr(buf, "://"))
        snprintf(full, sizeof full, "%s", buf);
    else
        snprintf(full, sizeof full, "https://%s", buf);
    if (strncasecmp(full, "https://", 8) && strncasecmp(full, "http://", 7) && strncasecmp(full, "ftp://", 6)) {
        snprintf(err, err_len, "Адрес должен начинаться с https://, http:// или ftp://");
        return -1;
    }
    const char *host = strstr(full, "://") + 3;
    const char *path = strchr(host, '/');
    size_t hl = path ? (size_t)(path - host) : strlen(host);
    if (!hl) {
        snprintf(err, err_len, "В адресе нет имени сервера");
        return -1;
    }
    if (check_host(host, hl, err, err_len) != 0)
        return -1;
    /* схема и хост — как есть; путь — кодируем всё, кроме допустимого */
    size_t o = 0;
    size_t head = (size_t)(host - full) + hl;
    if (head + 1 >= len) {
        snprintf(err, err_len, "Адрес слишком длинный");
        return -1;
    }
    memcpy(out, full, head);
    o = head;
    for (const char *s = path ? path : ""; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        int keep = isalnum(ch) || strchr("-._~/%:@!$&'()*+,;=", ch) != NULL;
        if (o + 4 >= len) {
            snprintf(err, err_len, "Адрес слишком длинный");
            return -1;
        }
        if (keep)
            out[o++] = (char)ch;
        else
            o += (size_t)snprintf(out + o, len - o, "%%%02X", ch);
    }
    out[o] = 0;
    while (o > head && out[o - 1] == '/')
        out[--o] = 0;
    return 0;
}

void remote_display(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    const char *at = strchr(p, '@');
    const char *slash = strchr(p, '/');
    if (at && (!slash || at < slash))
        p = at + 1;
    char dec[1024];
    net_url_decode(p, dec, sizeof dec);
    snprintf(out, len, "%s", dec);
}

/* Адрес папки или файла rel внутри папки сервера. */
static void join_url(const remote_cfg *c, const char *rel, int dir, char *out, size_t len)
{
    char enc[1100];
    net_url_encode(rel, 1, enc, sizeof enc);
    snprintf(out, len, "%s/%s%s", c->url, enc, dir && *enc ? "/" : "");
}

/* ---- Ошибки ---- */

static void explain(const net_response *rs, const char *op, char *err, size_t err_len)
{
    long st = rs->status;
    if (!strcmp(rs->error, "Отменено")) {
        snprintf(err, err_len, "Отменено");
        return;
    }
    if (st == 401)
        snprintf(err, err_len, "Неверный логин или пароль. Для Яндекс Диска нужен пароль приложения.");
    else if (st == 403)
        snprintf(err, err_len, "Сервер не дал доступа — проверьте права на папку.");
    else if ((st == 404 || st == 550) && !strcmp(op, "list"))
        snprintf(err, err_len, "На сервере нет такой папки — проверьте адрес.");
    else if (st == 404 || st == 550)
        snprintf(err, err_len, "Файла уже нет на сервере.");
    else if ((st == 405 || st == 501) && !strcmp(op, "list"))
        snprintf(err, err_len, "По этому адресу нет облачной папки (WebDAV) — проверьте адрес.");
    else if (st == 423)
        snprintf(err, err_len, "Папка на сервере занята, попробуйте позже.");
    else if (st == 429)
        snprintf(err, err_len, "Сервер просит подождать — попробуйте через несколько минут.");
    else if (st == 507 || st == 413)
        snprintf(err, err_len, "На сервере не хватает места.");
    else if (!strcmp(rs->error, "Сервер не отвечает") || !strcmp(rs->error, "Сервер слишком долго не отвечает"))
        snprintf(err, err_len, "%s. Проверьте адрес и что устройство и сервер в одной сети.", rs->error);
    else
        snprintf(err, err_len, "%s", rs->error[0] ? rs->error : "Ошибка сети");
}

static int no_ftp(const remote_cfg *c, char *err, size_t err_len)
{
    if (remote_is_ftp(c->url) && !net_has_protocol("ftp")) {
        snprintf(err, err_len, "В прошивке нет FTP — укажите адрес облачной папки (WebDAV): https://…");
        return 1;
    }
    return 0;
}

/* ---- FTP: разбор списков ---- */

static int month_index(const char *s)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    if (strlen(s) < 3)
        return -1;
    for (int i = 0; i < 12; i++)
        if (!strncasecmp(months + i * 3, s, 3) && (!s[3] || isspace((unsigned char)s[3])))
            return i;
    return -1;
}

static long long mlsd_time(const char *v)
{
    int y, mo, d, h, mi, se;
    if (sscanf(v, "%4d%2d%2d%2d%2d%2d", &y, &mo, &d, &h, &mi, &se) != 6)
        return 0;
    struct tm t;
    memset(&t, 0, sizeof t);
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = se;
    time_t r = timegm(&t);
    return r < 0 ? 0 : (long long)r;
}

int remote_parse_mlsd(const char *text, remote_entries *out)
{
    int added = 0;
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\r\n");
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        p += n;
        p += strspn(p, "\r\n");
        char *sp = strchr(line, ' ');
        if (!sp || sp == line)
            continue;
        *sp = 0;
        const char *name = sp + 1;
        int dir = -1;
        long long size = -1, mtime = 0;
        for (char *f = line; f && *f;) {
            char *next = strchr(f, ';');
            if (next)
                *next++ = 0;
            char *eq = strchr(f, '=');
            if (eq) {
                *eq = 0;
                const char *v = eq + 1;
                if (!strcasecmp(f, "type")) {
                    if (!strcasecmp(v, "file"))
                        dir = 0;
                    else if (!strcasecmp(v, "dir"))
                        dir = 1;
                    else
                        dir = -2; /* cdir, pdir, ссылки */
                } else if (!strcasecmp(f, "size")) {
                    size = atoll(v);
                } else if (!strcasecmp(f, "modify")) {
                    mtime = mlsd_time(v);
                }
            }
            f = next;
        }
        if (dir < 0 || !*name || !strcmp(name, ".") || !strcmp(name, ".."))
            continue;
        remote_entries_add(out, name, dir, dir ? -1 : size, mtime);
        added++;
    }
    return added;
}

/* Следующее слово строки (слова — через пробелы); *p — за ним. */
static const char *word(const char **p, char *buf, size_t len)
{
    while (**p == ' ' || **p == '\t')
        (*p)++;
    size_t n = strcspn(*p, " \t");
    if (!n)
        return NULL;
    snprintf(buf, len, "%.*s", (int)(n < len - 1 ? n : len - 1), *p);
    *p += n;
    return buf;
}

int remote_parse_list(const char *text, remote_entries *out)
{
    int added = 0;
    for (const char *p = text; *p;) {
        size_t n = strcspn(p, "\r\n");
        char line[1024];
        snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), p);
        p += n;
        p += strspn(p, "\r\n");

        const char *s = line;
        char w[512], t[9][64];
        int nt = 0;
        /* DOS (IIS): «11-24-26  10:30AM  <DIR>  имя» или «… 1234 имя» */
        if (isdigit((unsigned char)line[0]) && line[2] == '-') {
            char date[64], time_[64], kind[64];
            if (!word(&s, date, sizeof date) || !word(&s, time_, sizeof time_) || !word(&s, kind, sizeof kind))
                continue;
            int dir = !strcasecmp(kind, "<DIR>");
            while (*s == ' ')
                s++;
            if (!*s || !strcmp(s, ".") || !strcmp(s, ".."))
                continue;
            remote_entries_add(out, s, dir, dir ? -1 : atoll(kind), 0);
            added++;
            continue;
        }
        /* unix: права, ссылки, владелец, группа, размер, месяц, число, время или год, имя */
        if (!strchr("-dlbcps", line[0]) || strlen(line) < 12)
            continue;
        while (nt < 9 && word(&s, w, sizeof w))
            snprintf(t[nt++], sizeof t[0], "%.63s", w);
        int mi = -1; /* в каком слове месяц */
        for (int i = 4; i <= 5 && i < nt; i++)
            if (month_index(t[i]) >= 0 && isdigit((unsigned char)t[i - 1][0])) {
                mi = i;
                break;
            }
        if (mi < 0 || nt < mi + 3)
            continue;
        /* имя — всё после слова со временем/годом (mi + 2), пробелы внутри имени сохраняются */
        const char *name = line;
        for (int k = 0; k < mi + 3; k++) {
            while (*name == ' ' || *name == '\t')
                name++;
            name += strcspn(name, " \t");
        }
        while (*name == ' ' || *name == '\t')
            name++;
        if (!*name)
            continue;
        char nm[512];
        snprintf(nm, sizeof nm, "%s", name);
        if ((line[0] != '-' && line[0] != 'd') || !strcmp(nm, ".") || !strcmp(nm, ".."))
            continue; /* ссылки и устройства не трогаем */
        int dir = line[0] == 'd';
        remote_entries_add(out, nm, dir, dir ? -1 : atoll(t[mi - 1]), 0);
        added++;
    }
    return added;
}

/* ---- Запросы ---- */

static net_request base_request(const remote_cfg *c, const char *url, volatile int *cancel)
{
    net_request rq;
    memset(&rq, 0, sizeof rq);
    rq.url = url;
    rq.login = c->login[0] ? c->login : NULL;
    rq.password = c->password;
    rq.cancel = cancel;
    return rq;
}

static int list_ftp(const remote_cfg *c, const char *url, remote_entries *out, volatile int *cancel, char *err,
                    size_t err_len, long *status)
{
    for (int pass = 0; pass < 2; pass++) {
        net_request rq = base_request(c, url, cancel);
        rq.method = pass == 0 ? "MLSD" : NULL;
        rq.timeout_s = 90;
        net_response rs;
        int rc = net_perform(&rq, &rs);
        if (status)
            *status = rs.status;
        if (rc == 0) {
            const char *body = rs.body ? rs.body : ""; /* пустая папка — данных не было */
            int before = out->count;
            int n = pass == 0 ? remote_parse_mlsd(body, out) : remote_parse_list(body, out);
            int blank = !body[strspn(body, " \r\n\t")];
            net_response_free(&rs);
            if (n > 0 || blank || pass == 1)
                return 0;
            out->count = before; /* MLSD ответил непонятно — пробуем LIST */
            continue;
        }
        if (cancel && *cancel) {
            snprintf(err, err_len, "Отменено");
            net_response_free(&rs);
            return -1;
        }
        if (pass == 1 || rs.status == 530 || rs.status == 550) {
            explain(&rs, "list", err, err_len);
            net_response_free(&rs);
            return -1;
        }
        net_response_free(&rs); /* сервер не знает MLSD — LIST */
    }
    return -1;
}

int remote_list(const remote_cfg *c, const char *dir, remote_entries *out, volatile int *cancel, char *err,
                size_t err_len, long *status)
{
    if (status)
        *status = 0;
    err[0] = 0;
    if (no_ftp(c, err, err_len))
        return -1;
    char url[2200];
    join_url(c, dir, 1, url, sizeof url);
    if (!*dir)
        snprintf(url, sizeof url, "%s/", c->url);
    if (remote_is_ftp(c->url))
        return list_ftp(c, url, out, cancel, err, err_len, status);

    static const char *const headers[] = { "Depth: 1", "Content-Type: application/xml; charset=utf-8", NULL };
    net_request rq = base_request(c, url, cancel);
    rq.method = "PROPFIND";
    rq.headers = headers;
    rq.body = webdav_propfind_body;
    rq.body_len = strlen(webdav_propfind_body);
    rq.max_body = 32u << 20;
    rq.timeout_s = 120;
    net_response rs;
    int rc = net_perform(&rq, &rs);
    if (status)
        *status = rs.status;
    if (rc != 0) {
        explain(&rs, "list", err, err_len);
        net_response_free(&rs);
        return -1;
    }
    /* путь папки — по адресу после переадресаций */
    const char *fu = rs.final_url[0] ? rs.final_url : url;
    const char *scheme = strstr(fu, "://");
    const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
    char raw[2048], dec[2048];
    snprintf(raw, sizeof raw, "%.*s", path ? (int)strcspn(path, "?#") : 0, path ? path : "");
    net_url_decode(raw, dec, sizeof dec);
    size_t n = strlen(dec);
    while (n && dec[n - 1] == '/')
        dec[--n] = 0;
    int cnt = webdav_parse(rs.body ? rs.body : "", rs.len, dec, out);
    net_response_free(&rs);
    if (cnt < 0) {
        snprintf(err, err_len, "Это не облачная папка (WebDAV) — проверьте адрес.");
        return -1;
    }
    return 0;
}

int remote_get(const remote_cfg *c, const char *rel, const char *local, volatile int *cancel,
               void (*progress)(void *user, long long done, long long total), void *user, char *err,
               size_t err_len, long *status)
{
    err[0] = 0;
    if (no_ftp(c, err, err_len))
        return -1;
    char url[2200];
    join_url(c, rel, 0, url, sizeof url);
    net_request rq = base_request(c, url, cancel);
    rq.out_path = local;
    rq.progress = progress;
    rq.user = user;
    net_response rs;
    int rc = net_perform(&rq, &rs);
    if (status)
        *status = rs.status;
    if (rc != 0)
        explain(&rs, "get", err, err_len);
    net_response_free(&rs);
    return rc;
}

/* WebDAV: создать папки пути rel (все по очереди); 405 — уже есть. */
static void mkcol_parents(const remote_cfg *c, const char *rel, volatile int *cancel)
{
    char part[1100];
    snprintf(part, sizeof part, "%s", rel);
    for (char *s = part; (s = strchr(s, '/')) != NULL; s++) {
        *s = 0;
        char url[2200];
        join_url(c, part, 1, url, sizeof url);
        net_request rq = base_request(c, url, cancel);
        rq.method = "MKCOL";
        net_response rs;
        net_perform(&rq, &rs); /* ошибки неважны: если папки нет, упадёт сам PUT */
        net_response_free(&rs);
        *s = '/';
    }
}

int remote_put(const remote_cfg *c, const char *rel, const char *local, volatile int *cancel, char *err,
               size_t err_len)
{
    err[0] = 0;
    if (no_ftp(c, err, err_len))
        return -1;
    char url[2200];
    join_url(c, rel, 0, url, sizeof url);
    int ftp = remote_is_ftp(c->url);
    for (int pass = 0; pass < 2; pass++) {
        net_request rq = base_request(c, url, cancel);
        rq.upload_path = local;
        rq.create_dirs = ftp;
        rq.timeout_s = 300;
        net_response rs;
        int rc = net_perform(&rq, &rs);
        if (rc == 0) {
            net_response_free(&rs);
            return 0;
        }
        int retry = pass == 0 && !ftp && (rs.status == 409 || rs.status == 404 || rs.status == 405);
        explain(&rs, "put", err, err_len);
        net_response_free(&rs);
        if (!retry)
            return -1;
        mkcol_parents(c, rel, cancel);
    }
    return -1;
}

int remote_check(const remote_cfg *c, int *files, int *dirs, char *err, size_t err_len)
{
    remote_entries e = { NULL, 0, 0 };
    *files = *dirs = 0;
    int rc = remote_list(c, "", &e, NULL, err, err_len, NULL);
    for (int i = 0; rc == 0 && i < e.count; i++) {
        if (e.items[i].is_dir)
            (*dirs)++;
        else
            (*files)++;
    }
    remote_entries_free(&e);
    return rc;
}
