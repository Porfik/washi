/* _DEFAULT_SOURCE: getifaddrs, strcasecmp. */
#define _DEFAULT_SOURCE
#include "net.h"

#define CURL_DISABLE_TYPECHECK
#include <arpa/inet.h>
#include <ctype.h>
#include <curl/curl.h>
#include <dlfcn.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../base/log.h"
#include "../platform/platform.h"

/* ---- Адрес в сети ---- */

int net_local_ip(char *buf, size_t len)
{
    struct ifaddrs *list = NULL;
    if (len)
        buf[0] = 0;
    if (getifaddrs(&list) != 0)
        return -1;
    int best = 0; /* 2 — wlan (Wi‑Fi книги), 1 — другой не-loopback */
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !(i->ifa_flags & IFF_UP) ||
            (i->ifa_flags & IFF_LOOPBACK))
            continue;
        int rank = !strncmp(i->ifa_name, "wlan", 4) ? 2 : 1;
        if (rank <= best)
            continue;
        const struct sockaddr_in *sa = (const struct sockaddr_in *)(const void *)i->ifa_addr;
        if (inet_ntop(AF_INET, &sa->sin_addr, buf, (socklen_t)len))
            best = rank;
    }
    freeifaddrs(list);
    return best ? 0 : -1;
}

/* ---- libcurl через dlopen ---- */

static struct {
    CURLcode (*global_init)(long);
    CURL *(*easy_init)(void);
    CURLcode (*easy_setopt)(CURL *, CURLoption, ...);
    CURLcode (*easy_perform)(CURL *);
    void (*easy_cleanup)(CURL *);
    CURLcode (*easy_getinfo)(CURL *, CURLINFO, ...);
    const char *(*easy_strerror)(CURLcode);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
    curl_version_info_data *(*version_info)(CURLversion);
} C;
static int curl_ok;
static char curl_why[160];
static pthread_once_t curl_once = PTHREAD_ONCE_INIT;

static int sym(void *lib, const char *name, void *fn, size_t size)
{
    void *p = dlsym(lib, name);
    if (!p) {
        snprintf(curl_why, sizeof curl_why, "В libcurl нет %s", name);
        return -1;
    }
    memcpy(fn, &p, size); /* указатель на функцию из void * — по POSIX так можно */
    return 0;
}

static void curl_load(void)
{
    void *lib = dlopen("libcurl.so.4", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libcurl.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        snprintf(curl_why, sizeof curl_why, "Нет библиотеки libcurl");
        log_msg("сеть: нет libcurl: %s", dlerror());
        return;
    }
#define SYM(field, name)                                                                                \
    if (sym(lib, name, &C.field, sizeof C.field) != 0) {                                               \
        log_msg("сеть: %s", curl_why);                                                                 \
        return;                                                                                        \
    }
    SYM(global_init, "curl_global_init");
    SYM(easy_init, "curl_easy_init");
    SYM(easy_setopt, "curl_easy_setopt");
    SYM(easy_perform, "curl_easy_perform");
    SYM(easy_cleanup, "curl_easy_cleanup");
    SYM(easy_getinfo, "curl_easy_getinfo");
    SYM(easy_strerror, "curl_easy_strerror");
    SYM(slist_append, "curl_slist_append");
    SYM(slist_free_all, "curl_slist_free_all");
    SYM(version_info, "curl_version_info");
#undef SYM
    if (C.global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        snprintf(curl_why, sizeof curl_why, "libcurl не запустилась");
        log_msg("сеть: curl_global_init не удалась");
        return;
    }
    const curl_version_info_data *v = C.version_info(CURLVERSION_NOW);
    char protos[256] = "";
    for (int i = 0; v && v->protocols && v->protocols[i]; i++) {
        size_t n = strlen(protos);
        snprintf(protos + n, sizeof protos - n, "%s%s", n ? " " : "", v->protocols[i]);
    }
    log_msg("сеть: libcurl %s, %s; %s", v ? v->version : "?", v && v->ssl_version ? v->ssl_version : "без TLS",
            protos);
    curl_ok = 1;
}

int net_http_ready(char *why, size_t len)
{
    pthread_once(&curl_once, curl_load);
    if (!curl_ok && why && len)
        snprintf(why, len, "%s", curl_why);
    return curl_ok ? 0 : -1;
}

int net_has_protocol(const char *proto)
{
    if (net_http_ready(NULL, 0) != 0)
        return 0;
    const curl_version_info_data *v = C.version_info(CURLVERSION_NOW);
    for (int i = 0; v && v->protocols && v->protocols[i]; i++)
        if (!strcasecmp(v->protocols[i], proto))
            return 1;
    return 0;
}

/* ---- Запрос ---- */

typedef struct {
    const net_request *rq;
    net_response *rs;
    FILE *out;
    size_t cap, max;
    int too_big, write_failed;
} xfer;

static size_t on_write(char *data, size_t size, size_t n, void *user)
{
    xfer *x = user;
    size_t k = size * n;
    if (x->rq->cancel && *x->rq->cancel)
        return 0;
    if (x->out) {
        if (fwrite(data, 1, k, x->out) != k) {
            x->write_failed = 1;
            return 0;
        }
        return k;
    }
    if (x->rs->len + k > x->max) {
        x->too_big = 1;
        return 0;
    }
    if (x->rs->len + k + 1 > x->cap) {
        size_t cap = x->cap ? x->cap * 2 : 16384;
        while (cap < x->rs->len + k + 1)
            cap *= 2;
        char *b = realloc(x->rs->body, cap);
        if (!b) {
            x->too_big = 1;
            return 0;
        }
        x->rs->body = b;
        x->cap = cap;
    }
    memcpy(x->rs->body + x->rs->len, data, k);
    x->rs->len += k;
    x->rs->body[x->rs->len] = 0;
    return k;
}

/* filename="…" или filename*=UTF-8''…%XX… из Content-Disposition. */
static void disposition_name(const char *v, size_t n, char *out, size_t len)
{
    char line[1024];
    snprintf(line, sizeof line, "%.*s", (int)(n < sizeof line - 1 ? n : sizeof line - 1), v);
    const char *star = strstr(line, "filename*=");
    if (star) {
        const char *p = strstr(star, "''");
        p = p ? p + 2 : star + 10;
        char enc[1024];
        snprintf(enc, sizeof enc, "%.*s", (int)strcspn(p, ";\r\n"), p);
        net_url_decode(enc, out, len);
        return;
    }
    const char *p = strstr(line, "filename=");
    if (!p)
        return;
    p += 9;
    if (*p == '"') {
        p++;
        snprintf(out, len, "%.*s", (int)strcspn(p, "\"\r\n"), p);
    } else {
        snprintf(out, len, "%.*s", (int)strcspn(p, ";\r\n"), p);
    }
}

static size_t on_header(char *data, size_t size, size_t n, void *user)
{
    xfer *x = user;
    size_t k = size * n;
    if (k > 5 && !strncmp(data, "HTTP/", 5))
        x->rs->filename[0] = 0; /* новый ответ (после переадресации) */
    static const char cd[] = "content-disposition:";
    if (k > sizeof cd && !strncasecmp(data, cd, sizeof cd - 1))
        disposition_name(data + sizeof cd - 1, k - (sizeof cd - 1), x->rs->filename, sizeof x->rs->filename);
    return k;
}

static int on_progress(void *user, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
{
    xfer *x = user;
    if (x->rq->cancel && *x->rq->cancel)
        return 1;
    if (x->rq->progress) {
        if (x->rq->upload_path)
            x->rq->progress(x->rq->user, (long long)ulnow, ultotal > 0 ? (long long)ultotal : -1);
        else
            x->rq->progress(x->rq->user, (long long)dlnow, dltotal > 0 ? (long long)dltotal : -1);
    }
    return 0;
}

static void curl_error_text(CURLcode rc, const xfer *x, char *out, size_t len)
{
    switch (rc) {
    case CURLE_UNSUPPORTED_PROTOCOL: snprintf(out, len, "Такой адрес не поддерживается"); break;
    case CURLE_URL_MALFORMAT: snprintf(out, len, "Неверный адрес"); break;
    case CURLE_COULDNT_RESOLVE_HOST: snprintf(out, len, "Сервер не найден — проверьте адрес и сеть"); break;
    case CURLE_COULDNT_CONNECT: snprintf(out, len, "Сервер не отвечает"); break;
    case CURLE_OPERATION_TIMEDOUT: snprintf(out, len, "Сервер слишком долго не отвечает"); break;
    case CURLE_LOGIN_DENIED: snprintf(out, len, "Неверный логин или пароль"); break;
    case CURLE_REMOTE_FILE_NOT_FOUND: snprintf(out, len, "На сервере нет такого файла"); break;
    case CURLE_REMOTE_ACCESS_DENIED: snprintf(out, len, "Сервер не дал доступа"); break;
    case CURLE_ABORTED_BY_CALLBACK: snprintf(out, len, "Отменено"); break;
    case CURLE_TOO_MANY_REDIRECTS: snprintf(out, len, "Слишком много переадресаций"); break;
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
        snprintf(out, len, "Не удалось проверить сертификат сервера");
        break;
    case CURLE_SSL_CONNECT_ERROR:
        snprintf(out, len, "Не удалось установить защищённое соединение — возможно, сайт заблокирован провайдером");
        break;
    case CURLE_READ_ERROR: snprintf(out, len, "Не удалось прочитать файл"); break;
    case CURLE_WRITE_ERROR:
        if (x->rq->cancel && *x->rq->cancel)
            snprintf(out, len, "Отменено");
        else if (x->too_big)
            snprintf(out, len, "Ответ сервера слишком большой");
        else
            snprintf(out, len, "Не удалось записать файл (хватает ли места на устройстве?)");
        break;
    default: snprintf(out, len, "Ошибка сети: %s", C.easy_strerror(rc)); break;
    }
}

static void http_error_text(long status, char *out, size_t len)
{
    if (status == 451)
        snprintf(out, len, "Сайт заблокирован провайдером (451)");
    else if (status == 401)
        snprintf(out, len, "Неверный логин или пароль");
    else if (status == 403)
        snprintf(out, len, "Сервер не дал доступа");
    else if (status == 404)
        snprintf(out, len, "На сервере такого нет (404)");
    else if (status >= 500)
        snprintf(out, len, "Ошибка на сервере (%ld)", status);
    else
        snprintf(out, len, "Сервер ответил кодом %ld", status);
}

int net_perform(const net_request *rq, net_response *rs)
{
    memset(rs, 0, sizeof *rs);
    char why[160];
    if (net_http_ready(why, sizeof why) != 0) {
        snprintf(rs->error, sizeof rs->error, "%s", why);
        return -1;
    }
    CURL *h = C.easy_init();
    if (!h) {
        snprintf(rs->error, sizeof rs->error, "Не хватило памяти");
        return -1;
    }
    xfer x = { rq, rs, NULL, 0, rq->max_body ? rq->max_body : 8u << 20, 0, 0 };
    char part[1100] = "";
    FILE *in = NULL;
    struct curl_slist *hdr = NULL;
    int ok = -1;

    if (rq->out_path) {
        snprintf(part, sizeof part, "%s.part", rq->out_path);
        x.out = fopen(part, "wb");
        if (!x.out) {
            snprintf(rs->error, sizeof rs->error, "Не удалось создать файл на устройстве");
            goto done;
        }
    }
    C.easy_setopt(h, CURLOPT_URL, rq->url);
    C.easy_setopt(h, CURLOPT_NOSIGNAL, 1L); /* запросы идут в своих потоках */
    C.easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    C.easy_setopt(h, CURLOPT_MAXREDIRS, 10L);
    C.easy_setopt(h, CURLOPT_USERAGENT, "Washi/" WASHI_VERSION " (PocketBook)");
    C.easy_setopt(h, CURLOPT_CONNECTTIMEOUT, 20L);
    C.easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1L);
    C.easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 60L);
    if (rq->timeout_s > 0)
        C.easy_setopt(h, CURLOPT_TIMEOUT, (long)rq->timeout_s);
    C.easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); /* gzip, если libcurl умеет */
    C.easy_setopt(h, CURLOPT_WRITEFUNCTION, on_write);
    C.easy_setopt(h, CURLOPT_WRITEDATA, &x);
    C.easy_setopt(h, CURLOPT_HEADERFUNCTION, on_header);
    C.easy_setopt(h, CURLOPT_HEADERDATA, &x);
    C.easy_setopt(h, CURLOPT_XFERINFOFUNCTION, on_progress);
    C.easy_setopt(h, CURLOPT_XFERINFODATA, &x);
    C.easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    char cerr[CURL_ERROR_SIZE] = "";
    C.easy_setopt(h, CURLOPT_ERRORBUFFER, cerr); /* подробности для лога (сертификат, DNS…) */
    char ca[1100];
    struct stat st;
    snprintf(ca, sizeof ca, "%s/cacert.pem", pf_data_dir());
    if (stat(ca, &st) == 0)
        C.easy_setopt(h, CURLOPT_CAINFO, ca);
    if (rq->login) {
        C.easy_setopt(h, CURLOPT_USERNAME, rq->login);
        C.easy_setopt(h, CURLOPT_PASSWORD, rq->password ? rq->password : "");
        C.easy_setopt(h, CURLOPT_HTTPAUTH, (long)CURLAUTH_ANY);
    }
    for (int i = 0; rq->headers && rq->headers[i]; i++) {
        struct curl_slist *n = C.slist_append(hdr, rq->headers[i]);
        if (n)
            hdr = n;
    }
    if (hdr)
        C.easy_setopt(h, CURLOPT_HTTPHEADER, hdr);
    if (rq->body) {
        C.easy_setopt(h, CURLOPT_POSTFIELDS, rq->body);
        C.easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)rq->body_len);
    }
    if (rq->upload_path) {
        in = fopen(rq->upload_path, "rb");
        if (!in || fstat(fileno(in), &st) != 0) {
            snprintf(rs->error, sizeof rs->error, "Не удалось прочитать файл");
            goto done;
        }
        C.easy_setopt(h, CURLOPT_UPLOAD, 1L);
        C.easy_setopt(h, CURLOPT_READDATA, in); /* читает fread */
        C.easy_setopt(h, CURLOPT_INFILESIZE_LARGE, (curl_off_t)st.st_size);
        if (rq->create_dirs)
            C.easy_setopt(h, CURLOPT_FTP_CREATE_MISSING_DIRS, (long)CURLFTP_CREATE_DIR);
    }
    if (rq->method)
        C.easy_setopt(h, CURLOPT_CUSTOMREQUEST, rq->method);

    CURLcode rc = C.easy_perform(h);
    C.easy_getinfo(h, CURLINFO_RESPONSE_CODE, &rs->status);
    char *s = NULL;
    if (C.easy_getinfo(h, CURLINFO_CONTENT_TYPE, &s) == CURLE_OK && s)
        snprintf(rs->content_type, sizeof rs->content_type, "%s", s);
    s = NULL;
    if (C.easy_getinfo(h, CURLINFO_EFFECTIVE_URL, &s) == CURLE_OK && s)
        snprintf(rs->final_url, sizeof rs->final_url, "%s", s);
    int http = !strncasecmp(rs->final_url[0] ? rs->final_url : rq->url, "http", 4);
    if (rc != CURLE_OK)
        curl_error_text(rc, &x, rs->error, sizeof rs->error);
    else if (http && (rs->status < 200 || rs->status > 299))
        http_error_text(rs->status, rs->error, sizeof rs->error);
    else
        ok = 0;
    if (ok != 0)
        log_msg("сеть: %s %s — %s (curl %d, HTTP %ld%s%s)", rq->method ? rq->method : (rq->upload_path ? "PUT" : "GET"),
                rq->url, rs->error, (int)rc, rs->status, cerr[0] ? ": " : "", cerr);

done:
    if (x.out) {
        if (fclose(x.out) != 0 && ok == 0) {
            snprintf(rs->error, sizeof rs->error, "Не удалось записать файл (хватает ли места на устройстве?)");
            ok = -1;
        }
        if (ok == 0 && rename(part, rq->out_path) != 0) {
            snprintf(rs->error, sizeof rs->error, "Не удалось сохранить файл");
            ok = -1;
        }
        if (ok != 0)
            remove(part);
    }
    if (in)
        fclose(in);
    if (hdr)
        C.slist_free_all(hdr);
    C.easy_cleanup(h);
    if (ok != 0 && !rs->error[0])
        snprintf(rs->error, sizeof rs->error, "Ошибка сети");
    return ok;
}

void net_response_free(net_response *rs)
{
    free(rs->body);
    rs->body = NULL;
    rs->len = 0;
}

/* ---- Адреса ---- */

/* Длина «схема:» (с двоеточием) или 0. */
static size_t scheme_len(const char *s)
{
    if (!isalpha((unsigned char)s[0]))
        return 0;
    size_t i = 1;
    while (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')
        i++;
    return s[i] == ':' ? i + 1 : 0;
}

/* Убрать «.» и «..» из пути (path начинается с «/», без ? и #). */
static void remove_dots(char *path)
{
    char out[4096];
    size_t o = 0;
    const char *p = path;
    while (*p) {
        /* *p == '/': очередной отрезок до следующего «/» */
        const char *seg = p + 1;
        size_t n = strcspn(seg, "/");
        int last = seg[n] == 0;
        if (n == 1 && seg[0] == '.') {
            if (last && o + 1 < sizeof out)
                out[o++] = '/';
        } else if (n == 2 && seg[0] == '.' && seg[1] == '.') {
            while (o > 0 && out[o - 1] != '/')
                o--;
            if (o > 0)
                o--; /* и сам «/» перед отрезком */
            if (last && o + 1 < sizeof out)
                out[o++] = '/';
        } else if (o + n + 1 < sizeof out) {
            out[o++] = '/';
            memcpy(out + o, seg, n);
            o += n;
        }
        p = seg + n;
    }
    if (o == 0)
        out[o++] = '/';
    out[o] = 0;
    strcpy(path, out);
}

void net_url_resolve(const char *base, const char *href, char *out, size_t len)
{
    if (!len)
        return;
    char buf[4096];
    if (!href)
        href = "";
    if (scheme_len(href)) {
        snprintf(out, len, "%s", href);
        return;
    }
    size_t sl = scheme_len(base), auth_end = sl;
    if (base[sl] == '/' && base[sl + 1] == '/')
        auth_end = sl + 2 + strcspn(base + sl + 2, "/?#");
    if (!*href)
        snprintf(buf, sizeof buf, "%.*s", (int)strcspn(base, "#"), base);
    else if (href[0] == '/' && href[1] == '/')
        snprintf(buf, sizeof buf, "%.*s%s", (int)sl, base, href);
    else if (href[0] == '/')
        snprintf(buf, sizeof buf, "%.*s%s", (int)auth_end, base, href);
    else if (href[0] == '?')
        snprintf(buf, sizeof buf, "%.*s%s", (int)strcspn(base, "?#"), base, href);
    else if (href[0] == '#')
        snprintf(buf, sizeof buf, "%.*s%s", (int)strcspn(base, "#"), base, href);
    else {
        size_t path_end = auth_end + strcspn(base + auth_end, "?#"), slash = path_end;
        while (slash > auth_end && base[slash - 1] != '/')
            slash--;
        if (slash > auth_end)
            snprintf(buf, sizeof buf, "%.*s%s", (int)slash, base, href);
        else
            snprintf(buf, sizeof buf, "%.*s/%s", (int)auth_end, base, href);
    }
    /* Точки — только в пути: между «схема://хост» и ? или #. */
    size_t bsl = scheme_len(buf), bauth = bsl;
    if (buf[bsl] == '/' && buf[bsl + 1] == '/')
        bauth = bsl + 2 + strcspn(buf + bsl + 2, "/?#");
    if (buf[bauth] == '/') {
        size_t pend = bauth + strcspn(buf + bauth, "?#");
        char path[4096], rest[4096];
        snprintf(path, sizeof path, "%.*s", (int)(pend - bauth), buf + bauth);
        snprintf(rest, sizeof rest, "%s", buf + pend);
        remove_dots(path);
        snprintf(out, len, "%.*s%s%s", (int)bauth, buf, path, rest);
    } else {
        snprintf(out, len, "%s", buf);
    }
}

void net_url_encode(const char *s, int keep_slash, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    if (!len)
        return;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int plain = isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~' || (keep_slash && *p == '/');
        if (plain && *p < 0x80) {
            if (o + 1 >= len)
                break;
            out[o++] = (char)*p;
        } else {
            if (o + 3 >= len)
                break;
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 15];
        }
    }
    out[o] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    c = tolower(c);
    return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

void net_url_decode(const char *s, char *out, size_t len)
{
    size_t o = 0;
    if (!len)
        return;
    for (const char *p = s; *p && o + 1 < len; p++) {
        int hi, lo;
        if (*p == '%' && (hi = hexval((unsigned char)p[1])) >= 0 && (lo = hexval((unsigned char)p[2])) >= 0) {
            out[o++] = (char)(hi * 16 + lo);
            p += 2;
        } else {
            out[o++] = *p;
        }
    }
    out[o] = 0;
}

/* ---- Файлы ---- */

/* Длина, не больше max, по границе символа UTF-8. */
static size_t utf8_cut(const char *s, size_t n, size_t max)
{
    if (n <= max)
        return n;
    n = max;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80)
        n--;
    return n;
}

void net_safe_name(const char *name, char *out, size_t len)
{
    if (!len)
        return;
    const char *b = name ? name : "";
    for (const char *p = b; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1; /* только последняя часть пути */
    char tmp[1024];
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)b; *p && n + 1 < sizeof tmp; p++)
        tmp[n++] = *p < 0x20 || *p == 0x7f || strchr(":*?\"<>|", *p) ? '_' : (char)*p;
    tmp[n] = 0;
    size_t start = 0;
    while (tmp[start] == ' ' || tmp[start] == '.')
        start++;
    while (n > start && (tmp[n - 1] == ' ' || tmp[n - 1] == '.'))
        tmp[--n] = 0;
    const char *s = tmp + start;
    n -= start;
    if (n == 0) {
        snprintf(out, len, "book");
        return;
    }
    size_t max = len - 1 < 200 ? len - 1 : 200;
    if (n <= max) {
        snprintf(out, len, "%s", s);
        return;
    }
    const char *dot = strrchr(s, '.');
    size_t ext = dot && dot != s && strlen(dot) <= 12 ? strlen(dot) : 0;
    size_t stem = utf8_cut(s, n - ext, max - ext);
    snprintf(out, len, "%.*s%s", (int)stem, s, ext ? dot : "");
}

int net_mkdirs(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", dir);
    size_t n = strlen(p);
    while (n > 1 && p[n - 1] == '/')
        p[--n] = 0;
    for (char *c = p + 1; *c; c++) {
        if (*c != '/')
            continue;
        *c = 0;
        if (mkdir(p, 0755) != 0 && errno != EEXIST)
            return -1;
        *c = '/';
    }
    if (mkdir(p, 0755) != 0 && errno != EEXIST)
        return -1;
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode) ? 0 : -1;
}

int net_unique_path(const char *dir, const char *name, char *out, size_t len)
{
    if (net_mkdirs(dir) != 0)
        return -1;
    char safe[256];
    net_safe_name(name, safe, sizeof safe);
    struct stat st;
    snprintf(out, len, "%s/%s", dir, safe);
    if (stat(out, &st) != 0 && errno == ENOENT)
        return 0;
    const char *dot = strrchr(safe, '.');
    size_t stem = dot && dot != safe ? (size_t)(dot - safe) : strlen(safe);
    size_t n = strlen(safe);
    if (n > 8 && !strcasecmp(safe + n - 8, ".fb2.zip"))
        stem = n - 8; /* «книга (2).fb2.zip», а не «книга.fb2 (2).zip» */
    for (int i = 2; i < 1000; i++) {
        snprintf(out, len, "%s/%.*s (%d)%s", dir, (int)stem, safe, i, safe + stem);
        if (stat(out, &st) != 0 && errno == ENOENT)
            return 0;
    }
    return -1;
}
