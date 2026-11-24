/* _GNU_SOURCE: memmem, strcasestr, MSG_NOSIGNAL. */
#define _GNU_SOURCE
#include "httpd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../base/log.h"
#include "net.h"

#define HEADER_MAX 8192
#define HEADER_TIMEOUT_MS 15000
#define BODY_TIMEOUT_MS 20000 /* без байтов столько — обрыв */
#define RESERVE_BYTES (4LL << 20) /* свободного должно остаться сверх файла */
#define STACK_SIZE (256 * 1024)

#define MAX_HIDDEN 4
#define LS_MAX 4000       /* записей в одном списке папки */
#define TREE_DEPTH 32     /* глубже при удалении и подсчёте не идём */

struct httpd {
    xfer *log;
    char dir[1024];  /* папка приёма (куда без dir=) */
    char real_root[PATH_MAX]; /* корень библиотеки (настоящий путь) */
    char hidden[MAX_HIDDEN * 2][PATH_MAX]; /* закрытые папки (и их настоящие пути) */
    int nhidden;
    char def_rel[1100]; /* папка приёма относительно корня; has_def == 0 — она вне корня */
    int has_def;
    int fd, port;
    volatile int stop;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int conns; /* работающих потоков соединений */
};

typedef struct {
    httpd *h;
    int fd;
} conn;

/* ---- страница ---- */

/* Страница — src/net/web/index.html (HTML, стили и скрипт в одном файле); при сборке CMake кладёт её
 * в web_index.h массивом байт (с нулём в конце). */
#include "web_index.h"

/* ---- ответы ---- */

static int send_str(conn *c, const char *s)
{
    return xfer_send_all(c->fd, s, strlen(s), &c->h->stop);
}

static const char *status_text(int code)
{
    switch (code) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    case 507: return "Insufficient Storage";
    default: return "Error";
    }
}

static void reply(conn *c, int code, const char *type, const char *body, size_t len)
{
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     code, status_text(code), type, len);
    if (xfer_send_all(c->fd, head, (size_t)n, &c->h->stop) == 0 && len)
        xfer_send_all(c->fd, body, len, &c->h->stop);
}

/* Строка JSON: кавычки, обратная косая и управляющие — экранируются, UTF-8 как есть. */
static size_t json_str(char *out, size_t len, const char *s)
{
    size_t o = 0;
    for (; *s && o + 8 < len; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            out[o++] = '\\', out[o++] = (char)ch;
        else if (ch < 0x20)
            o += (size_t)snprintf(out + o, len - o, "\\u%04x", ch);
        else
            out[o++] = (char)ch;
    }
    out[o] = 0;
    return o;
}

/* 401: браузер сам покажет окно входа (логин любой, пароль — заданный) и повторит запрос. */
static void reply_unauthorized(conn *c)
{
    static const char body[] =
        "<!DOCTYPE html><html lang='ru'><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'><title>Нужен пароль</title></head>"
        "<body style='font:18px/1.5 system-ui,sans-serif;max-width:560px;margin:60px auto;padding:0 16px'>"
        "<h1>Нужен пароль</h1><p>Приём на устройстве защищён паролем. Введите его в окне входа "
        "(логин — любой). Если окно не появилось, обновите страницу.</p></body></html>";
    char head[320];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"InkPad\", charset=\"UTF-8\"\r\n"
                     "Content-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     sizeof body - 1);
    if (xfer_send_all(c->fd, head, (size_t)n, &c->h->stop) == 0)
        xfer_send_all(c->fd, body, sizeof body - 1, &c->h->stop);
}

static void reply_error(conn *c, int code, const char *text)
{
    char esc[512], body[640];
    json_str(esc, sizeof esc, text);
    int n = snprintf(body, sizeof body, "{\"error\":\"%s\"}", esc);
    reply(c, code, "application/json; charset=utf-8", body, (size_t)n);
}

static void reply_list(conn *c)
{
    xfer_snap *sn = malloc(sizeof *sn);
    if (!sn) {
        reply_error(c, 500, "Не хватило памяти");
        return;
    }
    xfer_snapshot(c->h->log, sn);
    size_t cap = 256 + (size_t)sn->ndone * 700;
    char *body = malloc(cap);
    if (!body) {
        free(sn);
        reply_error(c, 500, "Не хватило памяти");
        return;
    }
    long long fr = xfer_free_space(c->h->dir);
    size_t o = (size_t)snprintf(body, cap, "{\"free\":%lld,\"files\":[", fr);
    for (int i = 0; i < sn->ndone; i++) {
        char esc[600];
        json_str(esc, sizeof esc, sn->done[i].name);
        o += (size_t)snprintf(body + o, cap - o, "%s{\"name\":\"%s\",\"size\":%lld}", i ? "," : "", esc,
                              sn->done[i].size);
    }
    o += (size_t)snprintf(body + o, cap - o, "]}");
    reply(c, 200, "application/json; charset=utf-8", body, o);
    free(body);
    free(sn);
}

/* ---- запрос ---- */

typedef struct {
    char method[16];
    char target[2048];
    long long content_length; /* -1 — нет */
    int chunked, expect100;
    int xrw; /* пришёл заголовок X-Requested-With: washi (запрос из нашей страницы) */
    char auth[768]; /* значение Authorization: Basic …; пусто — не прислан */
    char *rest;       /* то, что прочитано сверх заголовка (начало тела) */
    size_t rest_len;
} request;

/* Читает заголовок в buf (с нулём в конце). 0 — готов; -1 — оборвалось или таймаут; -2 — слишком
 * длинный. *body_off — где в buf начинается тело, *got — сколько всего прочитано. */
static int read_header(conn *c, char *buf, size_t *body_off, size_t *got)
{
    size_t n = 0;
    double waited = 0;
    while (n < HEADER_MAX - 1) {
        int w = xfer_wait(c->fd, POLLIN, 1000, &c->h->stop);
        if (w < 0)
            return -1;
        if (w == 0) {
            waited += 1000;
            if (waited >= HEADER_TIMEOUT_MS)
                return -1;
            continue;
        }
        ssize_t r = recv(c->fd, buf + n, HEADER_MAX - 1 - n, 0);
        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (r <= 0)
            return -1;
        size_t from = n >= 3 ? n - 3 : 0;
        n += (size_t)r;
        buf[n] = 0;
        char *end = memmem(buf + from, n - from, "\r\n\r\n", 4);
        if (end) {
            *body_off = (size_t)(end - buf) + 4;
            *got = n;
            return 0;
        }
    }
    return -2;
}

static int parse_request(char *buf, size_t body_off, size_t got, request *rq)
{
    memset(rq, 0, sizeof *rq);
    rq->content_length = -1;
    char *line_end = strstr(buf, "\r\n");
    if (!line_end)
        return -1;
    *line_end = 0;
    char *sp1 = strchr(buf, ' ');
    if (!sp1)
        return -1;
    *sp1++ = 0;
    char *sp2 = strchr(sp1, ' ');
    if (!sp2)
        return -1;
    *sp2 = 0;
    if (strlen(buf) >= sizeof rq->method || strlen(sp1) >= sizeof rq->target)
        return -1;
    snprintf(rq->method, sizeof rq->method, "%s", buf);
    snprintf(rq->target, sizeof rq->target, "%s", sp1);
    for (char *l = line_end + 2; l < buf + body_off - 2;) {
        char *e = strstr(l, "\r\n");
        if (!e || e == l)
            break;
        *e = 0;
        char *colon = strchr(l, ':');
        if (colon) {
            *colon++ = 0;
            while (*colon == ' ' || *colon == '\t')
                colon++;
            if (!strcasecmp(l, "Content-Length")) {
                char *endp;
                long long v = strtoll(colon, &endp, 10);
                if (endp == colon || v < 0)
                    return -1;
                rq->content_length = v;
            } else if (!strcasecmp(l, "Transfer-Encoding") && strcasestr(colon, "chunked")) {
                rq->chunked = 1;
            } else if (!strcasecmp(l, "Authorization")) {
                if (strlen(colon) < sizeof rq->auth)
                    snprintf(rq->auth, sizeof rq->auth, "%s", colon);
            } else if (!strcasecmp(l, "X-Requested-With")) {
                rq->xrw = !strcasecmp(colon, "washi");
            } else if (!strcasecmp(l, "Expect") && strcasestr(colon, "100-continue")) {
                rq->expect100 = 1;
            }
        }
        l = e + 2;
    }
    rq->rest = buf + body_off;
    rq->rest_len = got - body_off;
    return 0;
}

/* ---- пароль ---- */

static int b64_val(int ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return ch - 'A';
    if (ch >= 'a' && ch <= 'z')
        return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9')
        return ch - '0' + 52;
    return ch == '+' ? 62 : ch == '/' ? 63 : -1;
}

/* «Basic base64(логин:пароль)» → пароль (логин любой). 0 — разобралось. */
static int basic_password(const char *auth, char *pass, size_t len)
{
    if (strncasecmp(auth, "Basic ", 6))
        return -1;
    auth += 6;
    while (*auth == ' ')
        auth++;
    unsigned char dec[600];
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (; *auth && *auth != '='; auth++) {
        int v = b64_val((unsigned char)*auth);
        if (v < 0 || o + 1 >= sizeof dec)
            return -1;
        acc = acc << 6 | (unsigned)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dec[o++] = (unsigned char)(acc >> bits & 255);
        }
    }
    dec[o] = 0;
    char *colon = strchr((char *)dec, ':');
    if (!colon || strlen(colon + 1) >= len)
        return -1;
    snprintf(pass, len, "%s", colon + 1);
    memset(dec, 0, sizeof dec);
    return 0;
}

/* Пускать ли. Пароля нет — да. Нет заголовка — 401 сразу (браузер спросит); неверный пароль — 401
 * после паузы (от перебора). */
static int authorized(conn *c, const request *rq)
{
    xfer *x = c->h->log;
    if (!xfer_has_password(x))
        return 1;
    char pass[512];
    int sent = rq->auth[0] && basic_password(rq->auth, pass, sizeof pass) == 0;
    if (sent && xfer_check_password(x, pass)) {
        memset(pass, 0, sizeof pass);
        return 1;
    }
    memset(pass, 0, sizeof pass);
    if (rq->auth[0]) {
        log_msg("приём: неверный пароль (браузер)");
        xfer_auth_pause(&c->h->stop);
    }
    reply_unauthorized(c);
    return 0;
}

/* Значение параметра name из строки запроса «a=1&name=%D0…», раскодированное. */
static int query_param(const char *target, const char *name, char *out, size_t len)
{
    const char *q = strchr(target, '?');
    size_t nl = strlen(name);
    out[0] = 0;
    if (!q)
        return -1;
    for (q++; *q;) {
        const char *amp = strchr(q, '&');
        size_t seg = amp ? (size_t)(amp - q) : strlen(q);
        if (seg > nl && !strncmp(q, name, nl) && q[nl] == '=') {
            char raw[2048];
            size_t vl = seg - nl - 1;
            if (vl >= sizeof raw)
                vl = sizeof raw - 1;
            memcpy(raw, q + nl + 1, vl);
            raw[vl] = 0;
            net_url_decode(raw, out, len);
            return 0;
        }
        q += seg + (amp ? 1 : 0);
    }
    return -1;
}

/* ---- папки библиотеки ---- */

/* Пути на странице — относительно корня библиотеки («Книги/Фантастика»), UTF-8. Внутрь корня
 * можно только по настоящему пути (realpath), выше корня, в закрытые папки (system, applications,
 * папка хаба) и по ссылкам наружу — нельзя. Как в ftpd.c. */

static int under(const char *path, const char *base)
{
    size_t n = strlen(base);
    return !strncasecmp(path, base, n) && (path[n] == 0 || path[n] == '/');
}

static int is_hidden(const httpd *h, const char *abs)
{
    for (int i = 0; i < h->nhidden; i++)
        if (under(abs, h->hidden[i]))
            return 1;
    return 0;
}

/* Закрыта ли внутри path какая-то из закрытых папок (путь — её предок): такую удалять и переименовывать нельзя. */
static int covers_hidden(const httpd *h, const char *abs)
{
    for (int i = 0; i < h->nhidden; i++)
        if (under(h->hidden[i], abs))
            return 1;
    return 0;
}

/* Папки прошивки и программ в корне (как в «Библиотеке»). */
static int root_reserved(const char *name)
{
    static const char *const names[] = { "system", "applications", "washi", "fonts", "screensavers",
                                         "System Volume Information" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcasecmp(name, names[i]))
            return 1;
    return 0;
}

static void add_hidden(httpd *h, const char *path)
{
    char real[PATH_MAX];
    if (h->nhidden < MAX_HIDDEN * 2)
        snprintf(h->hidden[h->nhidden++], PATH_MAX, "%s", path);
    if (realpath(path, real) && strcmp(real, path) && h->nhidden < MAX_HIDDEN * 2)
        snprintf(h->hidden[h->nhidden++], PATH_MAX, "%s", real);
}

/* Путь со страницы → нормализованный «a/b» (пустой — корень). -1 — нельзя: «..», «.», скрытые (с точки),
 * закрытые папки корня, обратная косая, управляющие знаки. */
static int rel_norm(const char *rel, char *out, size_t len)
{
    size_t o = 0;
    int first = 1;
    out[0] = 0;
    for (const char *c = rel; *c;) {
        while (*c == '/')
            c++;
        const char *e = c;
        while (*e && *e != '/')
            e++;
        size_t cl = (size_t)(e - c);
        if (cl == 0)
            break;
        if (c[0] == '.' || cl > 255)
            return -1;
        for (size_t i = 0; i < cl; i++)
            if ((unsigned char)c[i] < 0x20 || c[i] == 0x7f || c[i] == '\\')
                return -1;
        if (first && cl < 64) {
            char nm[64];
            memcpy(nm, c, cl);
            nm[cl] = 0;
            if (root_reserved(nm))
                return -1;
        }
        if (o + cl + 2 >= len)
            return -1;
        if (o)
            out[o++] = '/';
        memcpy(out + o, c, cl);
        o += cl;
        out[o] = 0;
        first = 0;
        c = e;
    }
    return 0;
}

/* Папка rel → настоящий путь real. 0 — можно; 403 — закрыта; 404 — нет такой. norm — нормализованный rel. */
static int dir_resolve(const httpd *h, const char *rel, char *norm, size_t nl, char *real, size_t rl)
{
    char abs[PATH_MAX + 1100], rp[PATH_MAX];
    if (rel_norm(rel, norm, nl) != 0)
        return 403;
    if (*norm)
        snprintf(abs, sizeof abs, "%s/%s", h->real_root, norm);
    else
        snprintf(abs, sizeof abs, "%s", h->real_root);
    if (!realpath(abs, rp))
        return errno == ENOENT || errno == ENOTDIR ? 404 : 403;
    if (!under(rp, h->real_root) || is_hidden(h, rp))
        return 403;
    struct stat st;
    if (stat(rp, &st) != 0 || !S_ISDIR(st.st_mode))
        return 404;
    snprintf(real, rl, "%s", rp);
    return 0;
}

/* Имя (последняя часть) от пути: в parent — всё до неё. */
static void rel_split(const char *norm, char *parent, size_t pl, const char **leaf)
{
    const char *sl = strrchr(norm, '/');
    if (sl) {
        snprintf(parent, pl, "%.*s", (int)(sl - norm), norm);
        *leaf = sl + 1;
    } else {
        parent[0] = 0;
        *leaf = norm;
    }
}

/* Файл или папка по пути со страницы (не корень, не ссылка, не закрытое, не предок закрытого).
 * 0 — можно: full — настоящий путь, st — lstat; иначе 403/404. parent_real — папка, где он лежит. */
static int entry_resolve(const httpd *h, const char *rel, char *norm, size_t nl, char *parent_real, size_t pl,
                         char *full, size_t fl, struct stat *st)
{
    char parent[1100];
    const char *leaf;
    if (rel_norm(rel, norm, nl) != 0 || !*norm)
        return 403;
    rel_split(norm, parent, sizeof parent, &leaf);
    char pn[1100];
    int rc = dir_resolve(h, parent, pn, sizeof pn, parent_real, pl);
    if (rc)
        return rc;
    snprintf(full, fl, "%s/%s", parent_real, leaf);
    if (lstat(full, st) != 0)
        return errno == ENOENT ? 404 : 403;
    if (S_ISLNK(st->st_mode) || !(S_ISDIR(st->st_mode) || S_ISREG(st->st_mode)))
        return 403;
    if (is_hidden(h, full) || (S_ISDIR(st->st_mode) && covers_hidden(h, full)))
        return 403;
    return 0;
}

/* Текст ошибки по коду dir_resolve/entry_resolve. */
static void reply_path_error(conn *c, int rc)
{
    if (rc == 404)
        reply_error(c, 404, "Такой папки или файла уже нет");
    else
        reply_error(c, 403, "Эта папка закрыта");
}

/* ---- список, подсчёт, создание, удаление, переименование ---- */

typedef struct {
    char *p;
    size_t n, cap;
} sbuf;

static int sb_add(sbuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static int sb_add(sbuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
    va_end(ap);
    if (n < 0)
        return -1;
    if (b->n + (size_t)n + 1 > b->cap) {
        size_t nc = (b->n + (size_t)n + 1) * 2;
        char *np = realloc(b->p, nc);
        if (!np)
            return -1;
        b->p = np;
        b->cap = nc;
        va_start(ap, fmt);
        n = vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
    }
    b->n += (size_t)n;
    return 0;
}

typedef struct {
    char name[256];
    long long size;
    long long mt; /* время изменения, секунды (0 — неизвестно) */
    int dir;
} lsent;

static int ls_cmp(const void *a, const void *b)
{
    const lsent *x = a, *y = b;
    if (x->dir != y->dir)
        return y->dir - x->dir;
    return strcasecmp(x->name, y->name);
}

/* GET /ls[?dir=]: папки и файлы (с размером). Без dir — папка приёма, если она внутри корня, иначе корень. */
static void do_ls(conn *c, const request *rq)
{
    httpd *h = c->h;
    char rel[1100], norm[1100], real[PATH_MAX];
    if (query_param(rq->target, "dir", rel, sizeof rel) != 0)
        snprintf(rel, sizeof rel, "%s", h->has_def ? h->def_rel : "");
    int rc = dir_resolve(h, rel, norm, sizeof norm, real, sizeof real);
    if (rc) {
        reply_path_error(c, rc);
        return;
    }
    DIR *dp = opendir(real);
    lsent *ents = malloc(LS_MAX * sizeof *ents);
    if (!dp || !ents) {
        if (dp)
            closedir(dp);
        free(ents);
        reply_error(c, 500, "Не удалось открыть папку");
        return;
    }
    int n = 0, at_root = !strcmp(real, h->real_root);
    struct dirent *e;
    while (n < LS_MAX && (e = readdir(dp))) {
        if (e->d_name[0] == '.' || (at_root && root_reserved(e->d_name)))
            continue;
        char full[PATH_MAX + 300];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", real, e->d_name);
        if (lstat(full, &st) != 0 || is_hidden(h, full))
            continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            continue;
        snprintf(ents[n].name, sizeof ents[n].name, "%s", e->d_name);
        ents[n].dir = S_ISDIR(st.st_mode);
        ents[n].size = ents[n].dir ? 0 : (long long)st.st_size;
        ents[n].mt = (long long)st.st_mtime;
        n++;
    }
    closedir(dp);
    qsort(ents, (size_t)n, sizeof *ents, ls_cmp);
    sbuf b = { malloc(4096), 0, 4096 };
    char esc[1200];
    int ok = b.p != NULL;
    if (ok) {
        json_str(esc, sizeof esc, norm);
        ok = sb_add(&b, "{\"dir\":\"%s\",\"free\":%lld,\"edit\":%s,\"def\":", esc, xfer_free_space(real),
                    xfer_allow_edit(h->log) ? "true" : "false") == 0;
    }
    if (ok && h->has_def) {
        json_str(esc, sizeof esc, h->def_rel);
        ok = sb_add(&b, "\"%s\"", esc) == 0;
    } else if (ok) {
        ok = sb_add(&b, "null") == 0;
    }
    for (int pass = 0; ok && pass < 2; pass++) {
        ok = sb_add(&b, pass ? "],\"files\":[" : ",\"dirs\":[") == 0;
        int first = 1;
        for (int i = 0; ok && i < n; i++) {
            if (ents[i].dir == pass)
                continue; /* первый проход — папки, второй — файлы */
            json_str(esc, sizeof esc, ents[i].name);
            ok = (pass ? sb_add(&b, "%s{\"name\":\"%s\",\"size\":%lld,\"mt\":%lld}", first ? "" : ",", esc,
                                      ents[i].size, ents[i].mt)
                       : sb_add(&b, "%s{\"name\":\"%s\",\"mt\":%lld}", first ? "" : ",", esc, ents[i].mt)) == 0;
            first = 0;
        }
    }
    if (ok)
        ok = sb_add(&b, "]}") == 0;
    if (ok)
        reply(c, 200, "application/json; charset=utf-8", b.p, b.n);
    else
        reply_error(c, 500, "Не хватило памяти");
    free(b.p);
    free(ents);
}

/* Сколько файлов в папке (с вложенными); cap — остановиться. Ссылки не берём. */
static long count_files(const char *dir, int depth, long cap)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return 0;
    long n = 0;
    struct dirent *e;
    while (n < cap && (e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[PATH_MAX + 300];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < TREE_DEPTH)
                n += count_files(full, depth + 1, cap - n);
        } else if (S_ISREG(st.st_mode) && e->d_name[0] != '.') {
            n++;
        }
    }
    closedir(dp);
    return n;
}

/* Удалить папку со всем внутри. Ссылки удаляются сами, наружу по ним не идём. 0 — готово. */
static int rm_tree(const char *dir, int depth)
{
    if (depth > TREE_DEPTH)
        return -1;
    DIR *dp = opendir(dir);
    if (!dp)
        return -1;
    int bad = 0;
    struct dirent *e;
    while ((e = readdir(dp))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char full[PATH_MAX + 300];
        struct stat st;
        snprintf(full, sizeof full, "%s/%s", dir, e->d_name);
        if (lstat(full, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (rm_tree(full, depth + 1) != 0)
                bad = 1;
        } else if (unlink(full) != 0) {
            bad = 1;
        }
    }
    closedir(dp);
    return bad || rmdir(dir) != 0 ? -1 : 0;
}

/* GET /count?path= : файлов в папке (для вопроса перед удалением). */
static void do_count(conn *c, const request *rq)
{
    char rel[1100], norm[1100], pr[PATH_MAX], full[PATH_MAX + 300];
    struct stat st;
    query_param(rq->target, "path", rel, sizeof rel);
    int rc = entry_resolve(c->h, rel, norm, sizeof norm, pr, sizeof pr, full, sizeof full, &st);
    if (rc) {
        reply_path_error(c, rc);
        return;
    }
    char body[64];
    int n = snprintf(body, sizeof body, "{\"files\":%ld}", S_ISDIR(st.st_mode) ? count_files(full, 0, 100000) : 1L);
    reply(c, 200, "application/json; charset=utf-8", body, (size_t)n);
}

/* Изменять можно только со страницы устройства (X-Requested-With) — чужая страница в браузере
 * не сможет послать такой запрос без разрешения (CORS). 1 — можно. */
static int mutation_ok(conn *c, const request *rq)
{
    if (!rq->xrw) {
        reply_error(c, 403, "Запрос не из страницы устройства");
        return 0;
    }
    return 1;
}

static int edit_ok(conn *c, const request *rq)
{
    if (!mutation_ok(c, rq))
        return 0;
    if (!xfer_allow_edit(c->h->log)) {
        reply_error(c, 403, "На устройстве не разрешено удалять и переименовывать из браузера");
        return 0;
    }
    return 1;
}

static void reply_ok_name(conn *c, const char *name)
{
    char esc[600], body[700];
    json_str(esc, sizeof esc, name);
    int n = snprintf(body, sizeof body, "{\"ok\":true,\"name\":\"%s\"}", esc);
    reply(c, 200, "application/json; charset=utf-8", body, (size_t)n);
}

/* Имя для новой папки или нового названия: из запроса через net_safe_name. 0 — есть. */
static int new_name(const request *rq, char *safe, size_t len)
{
    char raw[600];
    if (query_param(rq->target, "name", raw, sizeof raw) != 0)
        return -1;
    char *t = raw;
    while (*t == ' ')
        t++;
    if (!*t)
        return -1;
    net_safe_name(t, safe, len);
    return 0;
}

/* POST /mkdir?dir=&name= */
static void do_mkdir(conn *c, const request *rq)
{
    httpd *h = c->h;
    if (!mutation_ok(c, rq))
        return;
    char rel[1100], norm[1100], real[PATH_MAX], safe[256];
    query_param(rq->target, "dir", rel, sizeof rel);
    if (new_name(rq, safe, sizeof safe) != 0) {
        reply_error(c, 400, "Нужно название папки");
        return;
    }
    int rc = dir_resolve(h, rel, norm, sizeof norm, real, sizeof real);
    if (rc) {
        reply_path_error(c, rc);
        return;
    }
    if (!strcmp(real, h->real_root) && root_reserved(safe)) {
        reply_error(c, 409, "Это название занято устройством, выберите другое");
        return;
    }
    char full[PATH_MAX + 300];
    struct stat st;
    snprintf(full, sizeof full, "%s/%s", real, safe);
    if (lstat(full, &st) == 0) {
        reply_error(c, 409, "Такая папка или файл уже есть");
        return;
    }
    if (mkdir(full, 0755) != 0) {
        reply_error(c, 500, errno == ENOSPC ? "На устройстве кончилось место" : "Не удалось создать папку");
        return;
    }
    log_msg("приём: папка «%s» создана из браузера", full);
    reply_ok_name(c, safe);
}

/* POST /delete?path= */
static void do_delete(conn *c, const request *rq)
{
    httpd *h = c->h;
    if (!edit_ok(c, rq))
        return;
    char rel[1100], norm[1100], pr[PATH_MAX], full[PATH_MAX + 300];
    struct stat st;
    query_param(rq->target, "path", rel, sizeof rel);
    int rc = entry_resolve(h, rel, norm, sizeof norm, pr, sizeof pr, full, sizeof full, &st);
    if (rc) {
        reply_path_error(c, rc);
        return;
    }
    int isdir = S_ISDIR(st.st_mode);
    int r = isdir ? rm_tree(full, 0) : unlink(full);
    log_msg("приём: %s «%s» — %s (из браузера)", isdir ? "папка" : "файл", full, r == 0 ? "удалено" : "удалить не удалось");
    xfer_books_changed(h->log);
    if (r != 0) {
        reply_error(c, 500, "Не удалось удалить");
        return;
    }
    reply(c, 200, "application/json; charset=utf-8", "{\"ok\":true}", 11);
}

/* POST /rename?path=&name= */
static void do_rename(conn *c, const request *rq)
{
    httpd *h = c->h;
    if (!edit_ok(c, rq))
        return;
    char rel[1100], norm[1100], pr[PATH_MAX], full[PATH_MAX + 300], safe[256];
    struct stat st;
    query_param(rq->target, "path", rel, sizeof rel);
    if (new_name(rq, safe, sizeof safe) != 0) {
        reply_error(c, 400, "Нужно новое имя");
        return;
    }
    int rc = entry_resolve(h, rel, norm, sizeof norm, pr, sizeof pr, full, sizeof full, &st);
    if (rc) {
        reply_path_error(c, rc);
        return;
    }
    char to[PATH_MAX + 300];
    struct stat st2;
    snprintf(to, sizeof to, "%s/%s", pr, safe);
    if (!strcmp(pr, h->real_root) && root_reserved(safe)) {
        reply_error(c, 409, "Это имя занято устройством, выберите другое");
        return;
    }
    if (!strcmp(to, full)) {
        reply_ok_name(c, safe); /* то же имя */
        return;
    }
    /* Занято — но на FAT «книга.pdf» и «Книга.pdf» один файл: смена регистра разрешена. */
    if (lstat(to, &st2) == 0 && !(st2.st_dev == st.st_dev && st2.st_ino == st.st_ino)) {
        reply_error(c, 409, "Такое имя уже есть в этой папке");
        return;
    }
    if (rename(full, to) != 0) {
        reply_error(c, 500, "Не удалось переименовать");
        return;
    }
    log_msg("приём: «%s» → «%s» из браузера", full, to);
    xfer_books_changed(h->log);
    reply_ok_name(c, safe);
}

/* ---- загрузка ---- */

static void upload(conn *c, const request *rq)
{
    httpd *h = c->h;
    char name[512];
    if (query_param(rq->target, "name", name, sizeof name) != 0 || !*name)
        snprintf(name, sizeof name, "book");
    char safe[256];
    net_safe_name(name, safe, sizeof safe);

    if (rq->chunked || rq->content_length < 0) {
        reply_error(c, 411, "Нужен размер файла (Content-Length)");
        return;
    }
    long long total = rq->content_length;
    if (total == 0) {
        reply_error(c, 400, "Файл пустой");
        return;
    }
    /* Папка: dir= (относительно корня библиотеки) или, если не задана, папка приёма. */
    char tdir[PATH_MAX + 8], rel[1100], norm[1100];
    snprintf(tdir, sizeof tdir, "%s", h->dir);
    if (query_param(rq->target, "dir", rel, sizeof rel) == 0) {
        int rc = dir_resolve(h, rel, norm, sizeof norm, tdir, sizeof tdir);
        if (rc) {
            reply_path_error(c, rc);
            return;
        }
    }
    long long fr = xfer_free_space(tdir);
    if (fr >= 0 && total + RESERVE_BYTES > fr) {
        char msg[160], need[32], have[32];
        snprintf(need, sizeof need, "%lld МБ", (total + 1048575) >> 20);
        snprintf(have, sizeof have, "%lld МБ", fr >> 20);
        snprintf(msg, sizeof msg, "На устройстве не хватает места: нужно %s, свободно %s", need, have);
        log_msg("приём: %s — %s", safe, msg);
        reply_error(c, 507, msg);
        return;
    }
    if (net_mkdirs(tdir) != 0) {
        reply_error(c, 500, "Не удалось создать папку для книг");
        return;
    }
    char tmp[1100];
    int fd = xfer_tmp_open(tdir, tmp, sizeof tmp);
    if (fd < 0) {
        reply_error(c, 500, "Не удалось создать файл на устройстве");
        return;
    }
    int id = xfer_begin(h->log, safe, total, 0);
    if (id < 0) {
        close(fd);
        unlink(tmp);
        reply_error(c, 503, "Устройство принимает слишком много файлов сразу, повторите позже");
        return;
    }
    if (rq->expect100 && send_str(c, "HTTP/1.1 100 Continue\r\n\r\n") != 0)
        goto fail;

    char *buf = malloc(65536);
    if (!buf) {
        close(fd);
        unlink(tmp);
        xfer_end(h->log, id, 0, NULL, 0);
        reply_error(c, 500, "Не хватило памяти");
        return;
    }
    long long got = 0, reported = 0;
    size_t first = rq->rest_len;
    const char *src = rq->rest;
    const char *err = NULL;
    int code = 400;
    while (got < total) {
        size_t n;
        if (first) {
            n = first > (size_t)(total - got) ? (size_t)(total - got) : first;
            memcpy(buf, src, n);
            first = 0;
        } else {
            int w = xfer_wait(c->fd, POLLIN, BODY_TIMEOUT_MS, &h->stop);
            if (w <= 0) {
                err = w < 0 ? "Приём остановлен" : "Передача зависла";
                code = w < 0 ? 503 : 408;
                break;
            }
            size_t want = (size_t)(total - got) < 65536 ? (size_t)(total - got) : 65536;
            ssize_t r = recv(c->fd, buf, want, 0);
            if (r < 0 && (errno == EINTR || errno == EAGAIN))
                continue;
            if (r <= 0) {
                err = "Связь оборвалась";
                break;
            }
            n = (size_t)r;
        }
        for (size_t off = 0; off < n;) {
            ssize_t wr = write(fd, buf + off, n - off);
            if (wr < 0 && errno == EINTR)
                continue;
            if (wr <= 0) {
                err = errno == ENOSPC ? "На устройстве кончилось место" : "Не удалось записать файл на устройство";
                code = errno == ENOSPC ? 507 : 500;
                goto stopped;
            }
            off += (size_t)wr;
        }
        got += (long long)n;
        if (got - reported >= 256 * 1024 || got == total) {
            xfer_progress(h->log, id, got);
            reported = got;
        }
    }
stopped:
    free(buf);
    if (!err && fsync(fd) != 0)
        err = "Не удалось записать файл на устройство", code = 500;
    close(fd);
    if (err) {
        unlink(tmp);
        xfer_end(h->log, id, 0, NULL, 0);
        log_msg("приём: %s оборван (%lld из %lld): %s", safe, got, total, err);
        if (code != 408 && strcmp(err, "Связь оборвалась"))
            reply_error(c, code, err);
        return;
    }
    char final[1200];
    if (xfer_commit(tmp, tdir, safe, 1, final, sizeof final) != 0) {
        unlink(tmp);
        xfer_end(h->log, id, 0, NULL, 0);
        reply_error(c, 500, "Не удалось сохранить файл на устройстве");
        return;
    }
    const char *base = strrchr(final, '/');
    base = base ? base + 1 : final;
    xfer_end(h->log, id, 1, base, total);
    log_msg("приём: %s (%lld байт) по HTTP", final, total);
    char esc[600], body[700];
    json_str(esc, sizeof esc, base);
    int bn = snprintf(body, sizeof body, "{\"ok\":true,\"name\":\"%s\",\"size\":%lld}", esc, total);
    reply(c, 200, "application/json; charset=utf-8", body, (size_t)bn);
    return;
fail:
    close(fd);
    unlink(tmp);
    xfer_end(h->log, id, 0, NULL, 0);
}

static void handle(conn *c)
{
    httpd *h = c->h;
    xfer_touch(h->log);
    char *buf = malloc(HEADER_MAX);
    if (!buf)
        return;
    size_t body_off = 0, got = 0;
    int r = read_header(c, buf, &body_off, &got);
    request rq;
    if (r == -2) {
        reply_error(c, 431, "Заголовок запроса слишком длинный");
    } else if (r == 0 && parse_request(buf, body_off, got, &rq) != 0) {
        reply_error(c, 400, "Непонятный запрос");
    } else if (r == 0) {
        xfer_touch(h->log);
        if (!authorized(c, &rq)) {
            free(buf);
            return;
        }
        char path[2048];
        snprintf(path, sizeof path, "%s", rq.target);
        char *qm = strchr(path, '?');
        if (qm)
            *qm = 0;
        int is_get = !strcmp(rq.method, "GET") || !strcmp(rq.method, "HEAD");
        if (is_get && !strcmp(path, "/")) {
            reply(c, 200, "text/html; charset=utf-8", (const char *)web_index_html, sizeof web_index_html - 1);
        } else if (is_get && !strcmp(path, "/list")) {
            reply_list(c);
        } else if (is_get && !strcmp(path, "/ls")) {
            do_ls(c, &rq);
        } else if (is_get && !strcmp(path, "/count")) {
            do_count(c, &rq);
        } else if (!strcmp(path, "/mkdir") || !strcmp(path, "/delete") || !strcmp(path, "/rename")) {
            if (strcmp(rq.method, "POST"))
                reply_error(c, 405, "Нужен POST");
            else if (!strcmp(path, "/mkdir"))
                do_mkdir(c, &rq);
            else if (!strcmp(path, "/delete"))
                do_delete(c, &rq);
            else
                do_rename(c, &rq);
        } else if (is_get && !strcmp(path, "/favicon.ico")) {
            reply(c, 204, "image/x-icon", "", 0);
        } else if (!strcmp(path, "/upload")) {
            if (!strcmp(rq.method, "PUT") || !strcmp(rq.method, "POST"))
                upload(c, &rq);
            else
                reply_error(c, 405, "Нужен PUT или POST");
        } else if (is_get || !strcmp(rq.method, "PUT") || !strcmp(rq.method, "POST")) {
            reply_error(c, 404, "Такой страницы нет");
        } else {
            reply_error(c, 405, "Метод не поддерживается");
        }
    }
    free(buf);
}

static void *conn_thread(void *arg)
{
    conn *c = arg;
    httpd *h = c->h;
    handle(c);
    shutdown(c->fd, SHUT_WR);
    /* Дать клиенту дочитать ответ: вычитать то, что он ещё шлёт (иначе закрытие сбросит
     * соединение и ответ потеряется), но не дольше ~1,5 с. */
    char sink[4096];
    for (int i = 0; i < 8 && xfer_wait(c->fd, POLLIN, 200, &h->stop) == 1; i++)
        for (int k = 0; k < 64 && recv(c->fd, sink, sizeof sink, MSG_DONTWAIT) > 0; k++)
            ;
    close(c->fd);
    free(c);
    pthread_mutex_lock(&h->mu);
    h->conns--;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->mu);
    return NULL;
}

static void refuse_busy(int fd)
{
    static const char msg[] = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nRetry-After: 2\r\n"
                              "Connection: close\r\n\r\n";
    send(fd, msg, sizeof msg - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
}

static void *accept_thread(void *arg)
{
    httpd *h = arg;
    while (!h->stop) {
        if (xfer_wait(h->fd, POLLIN, 200, &h->stop) != 1)
            continue;
        int fd = accept(h->fd, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&h->mu);
        int busy = h->conns >= HTTPD_MAX_CONN;
        if (!busy)
            h->conns++;
        pthread_mutex_unlock(&h->mu);
        conn *c = busy ? NULL : malloc(sizeof *c);
        if (!c) {
            if (!busy) {
                pthread_mutex_lock(&h->mu);
                h->conns--;
                pthread_mutex_unlock(&h->mu);
            }
            refuse_busy(fd);
            continue;
        }
        c->h = h;
        c->fd = fd;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, STACK_SIZE);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (pthread_create(&t, &at, conn_thread, c) != 0) {
            pthread_mutex_lock(&h->mu);
            h->conns--;
            pthread_mutex_unlock(&h->mu);
            free(c);
            refuse_busy(fd);
        }
        pthread_attr_destroy(&at);
    }
    return NULL;
}

/* Папка приёма внутри корня → её путь относительно корня (def_rel); иначе has_def = 0. */
static void set_default(httpd *h)
{
    char real[PATH_MAX];
    h->has_def = 0;
    h->def_rel[0] = 0;
    if (!realpath(h->dir, real) || !under(real, h->real_root) || is_hidden(h, real))
        return;
    size_t rl = strcmp(h->real_root, "/") ? strlen(h->real_root) : 0;
    const char *rel = real + rl;
    while (*rel == '/')
        rel++;
    char norm[1100];
    if (rel_norm(rel, norm, sizeof norm) != 0)
        return;
    snprintf(h->def_rel, sizeof h->def_rel, "%s", norm);
    h->has_def = 1;
}

httpd *httpd_start(xfer *log, const char *dir, const char *root, const char *hidden_dir, int first_port)
{
    httpd *h = calloc(1, sizeof *h);
    if (!h)
        return NULL;
    h->log = log;
    snprintf(h->dir, sizeof h->dir, "%s", dir);
    /* Корень библиотеки; нет такой папки — только папка приёма. */
    if (!root || !*root || !realpath(root, h->real_root)) {
        net_mkdirs(h->dir);
        if (!realpath(h->dir, h->real_root))
            snprintf(h->real_root, sizeof h->real_root, "%s", h->dir);
    }
    char hp[PATH_MAX + 16];
    snprintf(hp, sizeof hp, "%s/system", h->real_root);
    add_hidden(h, hp);
    snprintf(hp, sizeof hp, "%s/applications", h->real_root);
    add_hidden(h, hp);
    if (hidden_dir && *hidden_dir) {
        snprintf(hp, sizeof hp, "%s", hidden_dir);
        size_t hl = strlen(hp);
        while (hl > 1 && hp[hl - 1] == '/')
            hp[--hl] = 0;
        add_hidden(h, hp);
    }
    pthread_mutex_init(&h->mu, NULL);
    pthread_cond_init(&h->cv, NULL);
    h->fd = xfer_listen(first_port, 10, &h->port);
    if (h->fd < 0) {
        log_msg("httpd: нет свободного порта с %d", first_port);
        free(h);
        return NULL;
    }
    if (net_mkdirs(h->dir) == 0)
        xfer_tmp_clean(h->dir);
    set_default(h);
    if (pthread_create(&h->thread, NULL, accept_thread, h) != 0) {
        close(h->fd);
        free(h);
        return NULL;
    }
    log_msg("httpd: порт %d, папка %s", h->port, h->dir);
    return h;
}

int httpd_port(const httpd *h)
{
    return h->port;
}

void httpd_stop(httpd *h)
{
    if (!h)
        return;
    h->stop = 1;
    pthread_join(h->thread, NULL);
    close(h->fd);
    pthread_mutex_lock(&h->mu);
    while (h->conns > 0)
        pthread_cond_wait(&h->cv, &h->mu);
    pthread_mutex_unlock(&h->mu);
    pthread_mutex_destroy(&h->mu);
    pthread_cond_destroy(&h->cv);
    log_msg("httpd: остановлен");
    free(h);
}
