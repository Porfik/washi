/* Приём по Wi‑Fi: HTTP- и FTP-серверы (httpd.c, ftpd.c) по настоящим сокетам на 127.0.0.1,
 * журнал приёма (xfer.c) и QR-код (qrcodegen). Файлы — во временной папке. */
#define _GNU_SOURCE
#include "../../src/base/secret.h"
#include "../../src/net/ftpd.h"
#include "../../src/net/httpd.h"
#include "../../src/net/xfer.h"
#include "../../src/vendor/qrcodegen.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

const char *pf_data_dir(void) /* net.c ищет там cacert.pem; secret.c — secret.key */
{
    return "/tmp";
}

int pf_device_secret(char *buf, size_t len) /* для secret.c: ключ пароля */
{
    snprintf(buf, len, "test-device|SN123");
    return 0;
}

static int fails;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            printf("НЕ ТАК (строка %d): ", __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
            fails++;                                                                                              \
        }                                                                                                         \
    } while (0)

static int dial(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { 10, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return fd;
}

static void send_all(int fd, const void *buf, size_t n)
{
    const char *p = buf;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w <= 0)
            return;
        p += w;
        n -= (size_t)w;
    }
}

/* Читает до закрытия или до таймаута. */
static size_t read_all(int fd, char *buf, size_t cap)
{
    size_t n = 0;
    ssize_t r;
    while (n + 1 < cap && (r = recv(fd, buf + n, cap - 1 - n, 0)) > 0)
        n += (size_t)r;
    buf[n] = 0;
    return n;
}

/* ---- HTTP ---- */

static char g_auth[200]; /* «логин:пароль» для запросов; пусто — без Authorization */

static void auth_header(char *out, size_t cap)
{
    static const char b[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out[0] = 0;
    if (!*g_auth)
        return;
    size_t n = strlen(g_auth), o = (size_t)snprintf(out, cap, "Authorization: Basic ");
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned char)g_auth[i] << 16 | (i + 1 < n ? (unsigned char)g_auth[i + 1] << 8 : 0) |
                     (i + 2 < n ? (unsigned char)g_auth[i + 2] : 0);
        out[o++] = b[v >> 18 & 63];
        out[o++] = b[v >> 12 & 63];
        out[o++] = i + 1 < n ? b[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? b[v & 63] : '=';
    }
    snprintf(out + o, cap - o, "\r\n");
}

/* PUT/POST name с телом body (len байт, но шлёт send_len); возвращает код ответа. */
static int http_put(int port, const char *method, const char *name, const char *body, size_t len, size_t send_len,
                    char *resp, size_t cap)
{
    int fd = dial(port);
    if (fd < 0)
        return -1;
    char head[900], ah[300];
    auth_header(ah, sizeof ah);
    int n = snprintf(head, sizeof head, "%s /upload?name=%s HTTP/1.1\r\nHost: x\r\n%sContent-Length: %zu\r\n\r\n", method,
                     name, ah, len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, send_len);
    if (send_len < len) { /* оборвать посреди тела */
        close(fd);
        return 0;
    }
    read_all(fd, resp, cap);
    close(fd);
    int code = 0;
    sscanf(resp, "HTTP/1.1 %d", &code);
    return code;
}

static int http_get(int port, const char *path, char *resp, size_t cap)
{
    int fd = dial(port);
    if (fd < 0)
        return -1;
    char head[600], ah[300];
    auth_header(ah, sizeof ah);
    int n = snprintf(head, sizeof head, "GET %s HTTP/1.1\r\nHost: x\r\n%s\r\n", path, ah);
    send_all(fd, head, (size_t)n);
    read_all(fd, resp, cap);
    close(fd);
    int code = 0;
    sscanf(resp, "HTTP/1.1 %d", &code);
    return code;
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static long long fsize(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long long)st.st_size : -1;
}

static int count_part_files(const char *dir)
{
    int n = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)))
        if (strstr(e->d_name, ".part"))
            n++;
    if (d)
        closedir(d);
    return n;
}

static void test_http(const char *base)
{
    char dl[600];
    snprintf(dl, sizeof dl, "%s/dl", base);
    xfer log;
    xfer_init(&log);
    httpd *h = httpd_start(&log, dl, NULL, NULL, 18080);
    CHECK(h != NULL, "httpd не запустился");
    if (!h)
        return;
    int port = httpd_port(h);
    static char resp[65536]; /* страница ~30 КБ */

    /* Занятый порт — следующий. */
    httpd *h2 = httpd_start(&log, dl, NULL, NULL, port);
    CHECK(h2 && httpd_port(h2) == port + 1, "второй сервер взял порт %d", h2 ? httpd_port(h2) : -1);
    httpd_stop(h2);

    CHECK(http_get(port, "/", resp, sizeof resp) == 200 && strstr(resp, "text/html") && strstr(resp, "<script>"),
          "страница");
    CHECK(strstr(resp, "Загрузить книги") != NULL && strstr(resp, "Новая папка") && strstr(resp, "id=\"crumbs\""),
          "в странице нет кнопки загрузки, крошек или новой папки");
    CHECK(strstr(resp, "pointer:fine") && strstr(resp, "FB2.ZIP") && strstr(resp, "DOCX") && strstr(resp, "overflow-wrap") &&
              strstr(resp, "Какие форматы подойдут?"),
          "в странице нет подсказки про форматы, перетаскивания только с мышью или переноса имён");
    CHECK(strstr(resp, "Нет связи с устройством") && strstr(resp, "Здесь пока пусто") && strstr(resp, "Отпустите") &&
              strstr(resp, "Разрешить удалять и переименовывать из браузера") && strstr(resp, "Это нельзя отменить"),
          "в странице нет состояний (нет связи, пусто, перетаскивание, подсказка про разрешение, вопрос удаления)");
    CHECK(!strstr(resp, "prompt(") && !strstr(resp, "confirm(") && !strstr(resp, "alert(") && !strstr(resp, "http://") &&
              !strstr(resp, "https://"),
          "в странице остались окна браузера или внешние ресурсы");
    CHECK(!strstr(resp, "на книге") && strlen(resp) < 50 * 1024, "в странице «на книге» вместо «устройство» или она больше 50 КБ");
    CHECK(http_get(port, "/nope", resp, sizeof resp) == 404, "404");

    /* Загрузка PUT и POST, русское имя (%D0…), занятое имя — «(2)». */
    char data[100000];
    for (size_t i = 0; i < sizeof data; i++)
        data[i] = (char)(i * 7 + 3);
    int code = http_put(port, "PUT", "%D0%9A%D0%BD%D0%B8%D0%B3%D0%B0.pdf", data, sizeof data, sizeof data, resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "\"ok\":true"), "PUT: %d %s", code, resp);
    char p[700];
    snprintf(p, sizeof p, "%s/Книга.pdf", dl);
    CHECK(fsize(p) == (long long)sizeof data, "файл Книга.pdf: %lld", fsize(p));
    FILE *f = fopen(p, "rb");
    char back[100000];
    CHECK(f && fread(back, 1, sizeof back, f) == sizeof back && !memcmp(back, data, sizeof data), "содержимое не совпало");
    if (f)
        fclose(f);
    code = http_put(port, "POST", "%D0%9A%D0%BD%D0%B8%D0%B3%D0%B0.pdf", data, 1000, 1000, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Книга (2).pdf", dl);
    CHECK(code == 200 && fsize(p) == 1000, "POST и «(2)»: %d %lld", code, fsize(p));

    /* Имя с «..» и «:» не выходит из папки. */
    code = http_put(port, "PUT", "..%2F..%2Fevil%3A.txt", data, 10, 10, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/evil_.txt", dl);
    CHECK(code == 200 && exists(p), "имя с «..»: %d", code);
    snprintf(p, sizeof p, "%s/../evil_.txt", dl);
    CHECK(!exists(p), "файл ушёл выше папки приёма");

    /* Без Content-Length, пустой файл. */
    int fd = dial(port);
    const char *nolen = "PUT /upload?name=x.pdf HTTP/1.1\r\nHost: x\r\n\r\n";
    send_all(fd, nolen, strlen(nolen));
    read_all(fd, resp, sizeof resp);
    close(fd);
    CHECK(strstr(resp, " 411 "), "без Content-Length: %.40s", resp);
    code = http_put(port, "PUT", "empty.pdf", "", 0, 0, resp, sizeof resp);
    CHECK(code == 400, "пустой файл: %d", code);

    /* Обрыв посреди тела — ни файла, ни временного. */
    http_put(port, "PUT", "torn.pdf", data, sizeof data, 30000, resp, sizeof resp);
    usleep(500000);
    snprintf(p, sizeof p, "%s/torn.pdf", dl);
    CHECK(!exists(p) && count_part_files(dl) == 0, "недописанный файл остался (.part: %d)", count_part_files(dl));

    /* Несколько соединений сразу. */
    int conns[4];
    for (int i = 0; i < 4; i++) {
        conns[i] = dial(port);
        char head[200];
        int n = snprintf(head, sizeof head, "PUT /upload?name=par%d.bin HTTP/1.1\r\nContent-Length: 50000\r\n\r\n", i);
        send_all(conns[i], head, (size_t)n);
        send_all(conns[i], data, 25000);
    }
    for (int i = 0; i < 4; i++) {
        send_all(conns[i], data, 25000);
        read_all(conns[i], resp, sizeof resp);
        close(conns[i]);
        snprintf(p, sizeof p, "%s/par%d.bin", dl, i);
        CHECK(fsize(p) == 50000, "параллельная загрузка %d: %lld", i, fsize(p));
    }

    CHECK(http_get(port, "/list", resp, sizeof resp) == 200 && strstr(resp, "\"name\":\"Книга.pdf\"") &&
              strstr(resp, "\"files\":["),
          "/list: %s", resp);

    xfer_snap sn;
    xfer_snapshot(&log, &sn);
    CHECK(sn.total_done == 7 && sn.nactive == 0, "журнал: принято %d, идут %d", sn.total_done, sn.nactive);
    CHECK(sn.ndone > 0 && sn.books_gen == sn.total_done, "books_gen %d", sn.books_gen);

    httpd_stop(h);
    CHECK(dial(port) < 0, "после остановки порт открыт");
    xfer_destroy(&log);
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Пароль: без него — как раньше (проверено в test_http); с ним — 401/200, загрузка, смена на ходу. */
static void test_http_password(const char *base)
{
    char dl[600], p[700];
    static char resp[65536];
    snprintf(dl, sizeof dl, "%s/dlpw", base);
    xfer log;
    xfer_init(&log);
    xfer_set_password(&log, "секрет 42");
    httpd *h = httpd_start(&log, dl, NULL, NULL, 18180);
    CHECK(h != NULL, "httpd не запустился");
    if (!h)
        return;
    int port = httpd_port(h);
    char data[5000];
    memset(data, 'x', sizeof data);

    g_auth[0] = 0;
    CHECK(http_get(port, "/", resp, sizeof resp) == 401 && strstr(resp, "WWW-Authenticate: Basic") &&
              strstr(resp, "Нужен пароль") && !strstr(resp, "<script>"),
          "страница без входа: %.60s", resp);
    CHECK(http_get(port, "/list", resp, sizeof resp) == 401, "/list без входа");
    CHECK(http_put(port, "PUT", "no.pdf", data, sizeof data, sizeof data, resp, sizeof resp) == 401, "загрузка без входа");
    usleep(300000);
    snprintf(p, sizeof p, "%s/no.pdf", dl);
    CHECK(!exists(p), "файл принят без пароля");

    snprintf(g_auth, sizeof g_auth, "любой:не тот");
    double t0 = now_s();
    CHECK(http_get(port, "/", resp, sizeof resp) == 401, "неверный пароль");
    CHECK(now_s() - t0 >= 0.9, "после неверного пароля нет паузы (%.2f с)", now_s() - t0);
    snprintf(g_auth, sizeof g_auth, "x:секрет 4"); /* префикс — не пароль */
    CHECK(http_get(port, "/", resp, sizeof resp) == 401, "префикс пароля пропущен");
    snprintf(g_auth, sizeof g_auth, "x:секрет 422");
    CHECK(http_get(port, "/", resp, sizeof resp) == 401, "пароль с лишним пропущен");

    snprintf(g_auth, sizeof g_auth, "любой:секрет 42");
    CHECK(http_get(port, "/", resp, sizeof resp) == 200 && strstr(resp, "<script>"), "страница с паролем");
    CHECK(http_get(port, "/list", resp, sizeof resp) == 200 && strstr(resp, "\"files\""), "/list с паролем");
    int code = http_put(port, "PUT", "yes.pdf", data, sizeof data, sizeof data, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/yes.pdf", dl);
    CHECK(code == 200 && fsize(p) == (long long)sizeof data, "загрузка с паролем: %d", code);
    snprintf(g_auth, sizeof g_auth, ":секрет 42"); /* пустой логин тоже годится */
    CHECK(http_get(port, "/", resp, sizeof resp) == 200, "пустой логин");

    /* Смена на ходу: старый перестаёт действовать, новый — сразу. */
    xfer_set_password(&log, "новый");
    snprintf(g_auth, sizeof g_auth, "u:секрет 42");
    CHECK(http_get(port, "/", resp, sizeof resp) == 401, "старый пароль после смены");
    snprintf(g_auth, sizeof g_auth, "u:новый");
    { int cc = http_get(port, "/", resp, sizeof resp); CHECK(cc == 200, "новый пароль: %d %.80s", cc, resp); }
    xfer_set_password(&log, NULL);
    g_auth[0] = 0;
    CHECK(http_get(port, "/", resp, sizeof resp) == 200, "пароль убран — без входа");

    httpd_stop(h);
    xfer_destroy(&log);
}

/* ---- FTP ---- */

typedef struct {
    int fd;
    char buf[4096];
} ftp;

/* Ответ управления: читает строки до «NNN␣…»; возвращает код, текст — в out. */
static int ftp_reply(ftp *c, char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (;;) {
        char line[1024];
        size_t l = 0;
        char ch;
        while (recv(c->fd, &ch, 1, 0) == 1 && ch != '\n')
            if (l + 1 < sizeof line && ch != '\r')
                line[l++] = ch;
        line[l] = 0;
        if (n + l + 2 < cap) {
            memcpy(out + n, line, l);
            n += l;
            out[n++] = '\n';
            out[n] = 0;
        }
        if (l >= 4 && line[3] == ' ' && line[0] >= '1' && line[0] <= '5')
            return atoi(line);
        if (l == 0)
            return -1;
    }
}

static int ftp_cmd(ftp *c, const char *cmd, char *out, size_t cap)
{
    char line[1100];
    int n = snprintf(line, sizeof line, "%s\r\n", cmd);
    send_all(c->fd, line, (size_t)n);
    return ftp_reply(c, out, cap);
}

/* PASV → открыть соединение данных. */
static int ftp_pasv(ftp *c)
{
    char r[512];
    if (ftp_cmd(c, "PASV", r, sizeof r) != 227)
        return -1;
    int a, b, cc, d, p1, p2;
    char *par = strchr(r, '(');
    if (!par || sscanf(par, "(%d,%d,%d,%d,%d,%d)", &a, &b, &cc, &d, &p1, &p2) != 6)
        return -1;
    return dial(p1 * 256 + p2);
}

static int ftp_login(ftp *c, int port)
{
    c->fd = dial(port);
    char r[512];
    if (c->fd < 0 || ftp_reply(c, r, sizeof r) != 220)
        return -1;
    if (ftp_cmd(c, "USER anonymous", r, sizeof r) != 331 || ftp_cmd(c, "PASS x@y", r, sizeof r) != 230)
        return -1;
    return 0;
}

static int ftp_list(ftp *c, const char *cmd, char *out, size_t cap)
{
    char r[512];
    int d = ftp_pasv(c);
    if (d < 0)
        return -1;
    int code = ftp_cmd(c, cmd, r, sizeof r);
    if (code != 150) {
        close(d);
        return code;
    }
    read_all(d, out, cap);
    close(d);
    return ftp_reply(c, r, sizeof r);
}

static int ftp_stor(ftp *c, const char *name, const char *data, size_t len, char *r, size_t rcap)
{
    int d = ftp_pasv(c);
    if (d < 0)
        return -1;
    char cmd[600];
    snprintf(cmd, sizeof cmd, "STOR %s", name);
    int code = ftp_cmd(c, cmd, r, rcap);
    if (code != 150) {
        close(d);
        return code;
    }
    send_all(d, data, len);
    close(d);
    return ftp_reply(c, r, rcap);
}

static void test_ftp(const char *base)
{
    char root[600], hub[600], p[900];
    snprintf(root, sizeof root, "%s/lib", base);
    snprintf(hub, sizeof hub, "%s/lib/hubdata", base);
    mkdir(root, 0755);
    snprintf(p, sizeof p, "%s/lib/Проза", base);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/lib/Проза/a.txt", base);
    FILE *f = fopen(p, "w");
    fputs("hello\n", f);
    fclose(f);
    snprintf(p, sizeof p, "%s/lib/SYSTEM", base); /* регистр не важен */
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/lib/SYSTEM/secret", base);
    f = fopen(p, "w");
    fputs("x", f);
    fclose(f);
    snprintf(p, sizeof p, "%s/lib/applications", base);
    mkdir(p, 0755);
    mkdir(hub, 0755);
    snprintf(p, sizeof p, "%s/outside.txt", base);
    f = fopen(p, "w");
    fputs("снаружи", f);
    fclose(f);

    xfer log;
    xfer_init(&log);
    ftpd *srv = ftpd_start(&log, root, hub, NULL, 12121);
    CHECK(srv != NULL, "ftpd не запустился");
    if (!srv)
        return;
    int port = ftpd_port(srv);
    char r[8192], listing[8192];
    ftp c;

    /* Без входа — только 530. */
    c.fd = dial(port);
    ftp_reply(&c, r, sizeof r);
    CHECK(ftp_cmd(&c, "LIST", r, sizeof r) == 530, "LIST до входа: %s", r);
    close(c.fd);

    CHECK(ftp_login(&c, port) == 0, "вход");
    CHECK(ftp_cmd(&c, "SYST", r, sizeof r) == 215, "SYST");
    CHECK(ftp_cmd(&c, "FEAT", r, sizeof r) == 211 && strstr(r, "UTF8") && strstr(r, "MLST"), "FEAT: %s", r);
    CHECK(ftp_cmd(&c, "OPTS UTF8 ON", r, sizeof r) == 200, "OPTS");
    CHECK(ftp_cmd(&c, "TYPE I", r, sizeof r) == 200, "TYPE");
    CHECK(ftp_cmd(&c, "PWD", r, sizeof r) == 257 && strstr(r, "\"/\""), "PWD: %s", r);

    /* Корень: Проза видна, SYSTEM, applications и папка хаба — нет. */
    CHECK(ftp_list(&c, "LIST", listing, sizeof listing) == 226, "LIST");
    CHECK(strstr(listing, "Проза") && !strcasestr(listing, "system") && !strstr(listing, "applications") &&
              !strstr(listing, "hubdata"),
          "список корня: %s", listing);
    CHECK(ftp_list(&c, "NLST", listing, sizeof listing) == 226 && strstr(listing, "outside") == NULL, "NLST: %s", listing);
    CHECK(ftp_list(&c, "MLSD", listing, sizeof listing) == 226 && strstr(listing, "type=dir;") && strstr(listing, "Проза"),
          "MLSD: %s", listing);
    CHECK(ftp_list(&c, "LIST Проза", listing, sizeof listing) == 226 && strstr(listing, "a.txt") && strstr(listing, "-rw-"),
          "LIST Проза: %s", listing);

    /* Закрытое и выход за корень. */
    const char *bad[] = { "CWD system", "CWD System", "CWD applications", "CWD hubdata", "CWD /hubdata", "CWD ..",
                          "CWD ../..", "CWD /..", "CWD Проза/../..", "CWD /Проза/../../lib", "CDUP",
                          "RETR ../outside.txt", "RETR system/secret", "SIZE ../outside.txt", "MDTM SYSTEM/secret",
                          "MKD system/x", "MKD ../x", "STOR ../x.txt", "STOR system/x.txt", "LIST system", "LIST ..",
                          "XCUP" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int code = ftp_cmd(&c, bad[i], r, sizeof r);
        CHECK(code >= 500 && code < 560, "%s пропущено: %d %s", bad[i], code, r);
    }
    CHECK(ftp_cmd(&c, "PWD", r, sizeof r) == 257 && strstr(r, "\"/\""), "после запретов PWD не корень: %s", r);

    /* Вход в папку, SIZE/MDTM, CDUP из подпапки. */
    CHECK(ftp_cmd(&c, "CWD Проза", r, sizeof r) == 250, "CWD Проза");
    CHECK(ftp_cmd(&c, "SIZE a.txt", r, sizeof r) == 213 && strstr(r, "213 6"), "SIZE: %s", r);
    CHECK(ftp_cmd(&c, "MDTM a.txt", r, sizeof r) == 213 && strlen(r) >= 18, "MDTM: %s", r);
    CHECK(ftp_cmd(&c, "CDUP", r, sizeof r) == 250, "CDUP из подпапки");

    /* RETR (и докачка REST). */
    {
        int d = ftp_pasv(&c);
        CHECK(ftp_cmd(&c, "REST 2", r, sizeof r) == 350, "REST");
        CHECK(ftp_cmd(&c, "RETR Проза/a.txt", r, sizeof r) == 150, "RETR");
        char got[100];
        read_all(d, got, sizeof got);
        close(d);
        CHECK(ftp_reply(&c, r, sizeof r) == 226 && !strcmp(got, "llo\n"), "RETR с 2-го байта: «%s»", got);
    }

    /* Папка и файл: русские имена, имя чинится под FAT32, перезапись запрещена. */
    CHECK(ftp_cmd(&c, "MKD Новая: папка", r, sizeof r) == 257 && strstr(r, "Новая_ папка"), "MKD: %s", r);
    CHECK(ftp_cmd(&c, "MKD Новая_ папка", r, sizeof r) == 550, "MKD повторно: %s", r);
    CHECK(ftp_cmd(&c, "CWD Новая_ папка", r, sizeof r) == 250, "CWD в новую");
    char big[300000];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (char)(i * 13);
    CHECK(ftp_stor(&c, "Книга №1.fb2", big, sizeof big, r, sizeof r) == 226, "STOR: %s", r);
    snprintf(p, sizeof p, "%s/lib/Новая_ папка/Книга №1.fb2", base);
    CHECK(fsize(p) == (long long)sizeof big, "файл после STOR: %lld", fsize(p));
    CHECK(ftp_stor(&c, "Книга №1.fb2", "x", 1, r, sizeof r) == 550 && fsize(p) == (long long)sizeof big,
          "перезапись разрешена: %s", r);
    CHECK(ftp_cmd(&c, "APPE Книга №1.fb2", r, sizeof r) == 550, "APPE");
    CHECK(ftp_cmd(&c, "DELE Книга №1.fb2", r, sizeof r) == 550 && strstr(r, "Удаление с устройства выключено") && exists(p), "DELE");
    CHECK(ftp_cmd(&c, "RMD /Новая_ папка", r, sizeof r) == 550, "RMD");
    CHECK(ftp_cmd(&c, "RNFR Книга №1.fb2", r, sizeof r) == 550, "RNFR");
    CHECK(ftp_cmd(&c, "RNTO other.fb2", r, sizeof r) == 550, "RNTO");
    char dirp[700];
    snprintf(dirp, sizeof dirp, "%s/lib/Новая_ папка", base);
    CHECK(count_part_files(dirp) == 0, "остался .part после STOR");

    /* Обрыв передачи: файла нет. */
    {
        int d = ftp_pasv(&c);
        CHECK(ftp_cmd(&c, "STOR torn.bin", r, sizeof r) == 150, "STOR torn");
        send_all(d, big, 1000);
        close(d); /* для FTP закрытие данных = конец файла: 1000 байт принято целиком */
        ftp_reply(&c, r, sizeof r);
    }
    /* Активный режим не поддерживается. */
    CHECK(ftp_cmd(&c, "PORT 127,0,0,1,4,1", r, sizeof r) == 502, "PORT");
    CHECK(ftp_cmd(&c, "EPSV", r, sizeof r) == 229 && strstr(r, "|||"), "EPSV: %s", r);
    CHECK(ftp_cmd(&c, "BOGUS", r, sizeof r) == 502, "неизвестная команда");
    CHECK(ftp_cmd(&c, "NOOP", r, sizeof r) == 200, "NOOP");
    CHECK(ftp_cmd(&c, "QUIT", r, sizeof r) == 221, "QUIT");
    close(c.fd);

    /* Много клиентов сразу: лишним — 421. */
    ftp many[FTPD_MAX_CLIENTS + 1];
    int refused = 0;
    for (int i = 0; i <= FTPD_MAX_CLIENTS; i++) {
        many[i].fd = dial(port);
        if (ftp_reply(&many[i], r, sizeof r) == 421)
            refused++;
    }
    CHECK(refused == 1, "отказано %d из %d", refused, FTPD_MAX_CLIENTS + 1);
    for (int i = 0; i <= FTPD_MAX_CLIENTS; i++)
        close(many[i].fd);

    xfer_snap sn;
    xfer_snapshot(&log, &sn);
    CHECK(sn.total_done >= 1 && !strcmp(sn.done[sn.ndone - 1].name, "Книга №1.fb2"), "журнал FTP: %d принято", sn.total_done);

    /* Начальная папка: после входа клиент уже в папке приёма, «..» ведёт выше, до корня. */
    {
        snprintf(p, sizeof p, "%s/lib/Проза/", base);
        ftpd *h1 = ftpd_start(&log, root, hub, p, 12150);
        CHECK(h1 && !strcmp(ftpd_home(h1), "/Проза"), "home: %s", h1 ? ftpd_home(h1) : "(нет)");
        if (h1) {
            CHECK(ftp_login(&c, ftpd_port(h1)) == 0, "вход в home");
            CHECK(ftp_cmd(&c, "PWD", r, sizeof r) == 257 && strstr(r, "\"/Проза\""), "PWD в home: %s", r);
            CHECK(ftp_list(&c, "LIST", listing, sizeof listing) == 226 && strstr(listing, "a.txt"), "список home: %s", listing);
            CHECK(ftp_stor(&c, "fromhome.pdf", "abc", 3, r, sizeof r) == 226, "STOR в home: %s", r);
            snprintf(p, sizeof p, "%s/lib/Проза/fromhome.pdf", base);
            CHECK(fsize(p) == 3, "файл из home лёг не в папку приёма");
            CHECK(ftp_cmd(&c, "CDUP", r, sizeof r) == 250 && ftp_cmd(&c, "PWD", r, sizeof r) == 257 && strstr(r, "\"/\""),
                  "CDUP из home: %s", r);
            CHECK(ftp_cmd(&c, "CWD ..", r, sizeof r) >= 500, "выше корня: %s", r);
            ftp_cmd(&c, "QUIT", r, sizeof r);
            close(c.fd);
            ftpd_stop(h1);
        }
        /* Папка вне корня, скрытая или не папка — клиент в корне. */
        const char *bad_home[] = { "/tmp", hub, "/nonexistent-dir-xyz", base };
        for (size_t i = 0; i < sizeof bad_home / sizeof *bad_home; i++) {
            ftpd *h2 = ftpd_start(&log, root, hub, bad_home[i], 12160);
            CHECK(h2 && !strcmp(ftpd_home(h2), "/"), "home %s: %s", bad_home[i], h2 ? ftpd_home(h2) : "(нет)");
            ftpd_stop(h2);
        }
        snprintf(p, sizeof p, "%s/lib", base);
        ftpd *h3 = ftpd_start(&log, root, hub, p, 12170); /* сам корень */
        CHECK(h3 && !strcmp(ftpd_home(h3), "/"), "home = корень: %s", h3 ? ftpd_home(h3) : "(нет)");
        ftpd_stop(h3);
    }

    /* Пароль: PASS должен совпасть (USER любой), после 3 неверных — закрыть; смена — сразу. */
    {
        xfer_set_password(&log, "секрет 42");
        c.fd = dial(port);
        CHECK(ftp_reply(&c, r, sizeof r) == 220, "приветствие");
        CHECK(ftp_cmd(&c, "USER кто-угодно", r, sizeof r) == 331 && strstr(r, "Нужен пароль"), "USER с паролем: %s", r);
        double t0 = now_s();
        CHECK(ftp_cmd(&c, "PASS не тот", r, sizeof r) == 530, "неверный PASS: %s", r);
        CHECK(now_s() - t0 >= 0.9, "после неверного PASS нет паузы (%.2f с)", now_s() - t0);
        CHECK(ftp_cmd(&c, "PWD", r, sizeof r) == 530, "команда без входа");
        CHECK(ftp_cmd(&c, "PASS секрет 42", r, sizeof r) == 230, "верный PASS после неверного: %s", r);
        CHECK(ftp_cmd(&c, "PWD", r, sizeof r) == 257, "PWD после входа");
        xfer_set_password(&log, "новый"); /* вошедшему — заново */
        CHECK(ftp_cmd(&c, "PWD", r, sizeof r) == 530, "после смены пароля вход действует: %s", r);
        close(c.fd);

        c.fd = dial(port);
        ftp_reply(&c, r, sizeof r);
        ftp_cmd(&c, "USER a", r, sizeof r);
        CHECK(ftp_cmd(&c, "PASS 1", r, sizeof r) == 530 && ftp_cmd(&c, "PASS 2", r, sizeof r) == 530, "две неверные");
        CHECK(ftp_cmd(&c, "PASS 3", r, sizeof r) == 530, "третья неверная");
        char ch;
        CHECK(recv(c.fd, &ch, 1, 0) == 0, "после 3 неверных соединение не закрыто");
        close(c.fd);
        CHECK(ftp_login(&c, port) != 0, "вход с «PASS x@y» при заданном пароле");
        close(c.fd);

        xfer_set_password(&log, NULL); /* убран — как раньше: любой пароль */
        CHECK(ftp_login(&c, port) == 0, "вход без пароля");
        ftp_cmd(&c, "QUIT", r, sizeof r);
        close(c.fd);
    }

    ftpd_stop(srv);
    CHECK(dial(port) < 0, "после остановки порт открыт");
    xfer_destroy(&log);
}

/* Файл пароля: зашифрован, читается обратно, «убрать» удаляет файл. */
static void test_pass_file(const char *base)
{
    char f[600], back[256], line[1024];
    snprintf(f, sizeof f, "%s/recv.txt", base);
    CHECK(xfer_pass_load(f, back, sizeof back) == 0 && !back[0], "нет файла — нет пароля");
    CHECK(xfer_pass_save(f, "секрет 42") == 0, "запись пароля");
    FILE *fp = fopen(f, "r");
    int found = 0, plain = 0;
    while (fp && fgets(line, sizeof line, fp)) {
        found |= !strncmp(line, "password=enc1:", 14);
        /* Кириллицы в base64 не бывает, а «42» в случайном шифротексте иногда встречается. */
        plain |= strstr(line, "секрет") != NULL;
    }
    if (fp)
        fclose(fp);
    CHECK(found && !plain, "recv.txt: зашифрованного нет или пароль открытым текстом");
    CHECK(xfer_pass_load(f, back, sizeof back) == 1 && !strcmp(back, "секрет 42"), "чтение пароля: «%s»", back);
    /* Старый открытый пароль читается как есть. */
    fp = fopen(f, "w");
    fputs("password=plain-old\n", fp);
    fclose(fp);
    CHECK(xfer_pass_load(f, back, sizeof back) == 1 && !strcmp(back, "plain-old"), "открытый пароль");
    /* Испорченный не открывается. */
    fp = fopen(f, "w");
    fputs("password=enc1:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\n", fp);
    fclose(fp);
    CHECK(xfer_pass_load(f, back, sizeof back) == -1 && strlen(back) >= 16, "испорченный пароль: не «закрыто»");
    {
        /* Закрыто по умолчанию: серверы с этим случайным паролем никого не пускают (ни пустой, ни «x»). */
        char back2[256], dl[600], resp[4096];
        xfer_pass_load(f, back2, sizeof back2);
        CHECK(strcmp(back, back2), "случайный пароль повторился");
        snprintf(dl, sizeof dl, "%s/dllost", base);
        xfer lg;
        xfer_init(&lg);
        xfer_set_password(&lg, back);
        httpd *h = httpd_start(&lg, dl, NULL, NULL, 18280);
        ftpd *fs = ftpd_start(&lg, base, NULL, NULL, 12280);
        CHECK(h && fs, "серверы не запустились");
        if (h && fs) {
            g_auth[0] = 0;
            CHECK(http_get(httpd_port(h), "/", resp, sizeof resp) == 401, "без пароля пустили");
            snprintf(g_auth, sizeof g_auth, "u:");
            CHECK(http_get(httpd_port(h), "/", resp, sizeof resp) == 401, "с пустым паролем пустили");
            snprintf(g_auth, sizeof g_auth, "u:x");
            CHECK(http_get(httpd_port(h), "/", resp, sizeof resp) == 401, "с чужим паролем пустили");
            g_auth[0] = 0;
            ftp c;
            c.fd = dial(ftpd_port(fs));
            ftp_reply(&c, resp, sizeof resp);
            ftp_cmd(&c, "USER a", resp, sizeof resp);
            CHECK(ftp_cmd(&c, "PASS x@y", resp, sizeof resp) == 530, "FTP пустил с чужим паролем");
            close(c.fd);
            xfer_set_password(&lg, NULL); /* «Убрать пароль» — снова открыто */
            CHECK(http_get(httpd_port(h), "/", resp, sizeof resp) == 200, "после «убрать» закрыто");
        }
        if (fs)
            ftpd_stop(fs);
        if (h)
            httpd_stop(h);
        xfer_destroy(&lg);
    }
    CHECK(xfer_pass_save(f, "") == 0 && !exists(f), "убрать пароль — файл удалён");
}


/* ---- папки на странице: список, загрузка в папку, создание, удаление, переименование ---- */

/* Любой запрос: method target, с заголовком X-Requested-With (xrw) и телом. Код ответа. */
static int http_call(int port, const char *method, const char *target, int xrw, const char *body, size_t len,
                     char *resp, size_t cap)
{
    int fd = dial(port);
    if (fd < 0)
        return -1;
    char head[2400], ah[300];
    auth_header(ah, sizeof ah);
    int n = snprintf(head, sizeof head, "%s %s HTTP/1.1\r\nHost: x\r\n%s%sContent-Length: %zu\r\n\r\n", method, target, ah,
                     xrw ? "X-Requested-With: washi\r\n" : "", len);
    send_all(fd, head, (size_t)n);
    if (len)
        send_all(fd, body, len);
    read_all(fd, resp, cap);
    close(fd);
    int code = 0;
    sscanf(resp, "HTTP/1.1 %d", &code);
    return code;
}

static void mkfile(const char *path, size_t n)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (size_t i = 0; i < n; i++)
        fputc('x', f);
    fclose(f);
}

static void test_folders(const char *base)
{
    char lib[600], p[900], r[1000];
    static char resp[65536];
    snprintf(lib, sizeof lib, "%s/wlib", base);
    snprintf(p, sizeof p, "%s/outside", base);
    mkdir(lib, 0755);
    mkdir(p, 0755);
    snprintf(p, sizeof p, "%s/outside/secret.txt", base);
    mkfile(p, 10);
    const char *dirs[] = { "Downloads", "Books", "Books/Sci", "system", "applications", "hub", ".hidden", "Wrap",
                           "Wrap/hubdata" };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        snprintf(p, sizeof p, "%s/%s", lib, dirs[i]);
        mkdir(p, 0755);
    }
    const char *files[] = { "Books/a.pdf", "Books/b.pdf", "Books/Sci/c.epub", "system/fw.bin", "hub/x.txt", ".hidden/y.txt",
                            "Wrap/hubdata/z.txt", "Wrap/w.pdf" };
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(p, sizeof p, "%s/%s", lib, files[i]);
        mkfile(p, 1000 + i);
    }
    snprintf(p, sizeof p, "%s/outside", base);
    snprintf(r, sizeof r, "%s/link", lib);
    CHECK(symlink(p, r) == 0, "ссылка не создалась");

    char dl[700], hub[700], wrap_hub[700];
    snprintf(dl, sizeof dl, "%s/Downloads", lib);
    snprintf(hub, sizeof hub, "%s/hub", lib);
    snprintf(wrap_hub, sizeof wrap_hub, "%s/Wrap/hubdata", lib);
    xfer log;
    xfer_init(&log);
    httpd *h = httpd_start(&log, dl, lib, wrap_hub, 18380);
    if (!h) {
        CHECK(0, "httpd для папок не запустился");
        xfer_destroy(&log);
        return;
    }
    int port = httpd_port(h);
    g_auth[0] = 0;
    int code;

    /* Список */
    code = http_get(port, "/ls", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "\"dir\":\"Downloads\"") && strstr(resp, "\"def\":\"Downloads\"") &&
              strstr(resp, "\"edit\":false"),
          "/ls без dir: %d %s", code, resp);
    code = http_get(port, "/ls?dir=", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "{\"name\":\"Books\",\"mt\":") && strstr(resp, "{\"name\":\"Downloads\",\"mt\":") &&
              strstr(resp, "{\"name\":\"Wrap\",\"mt\":") && !strstr(resp, "\"system\"") && !strstr(resp, "applications") &&
              !strstr(resp, "hubdata") && !strstr(resp, "hidden") && !strstr(resp, "link") && !strstr(resp, "outside"),
          "корень: %d %s", code, resp);
    code = http_get(port, "/ls?dir=Books", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "\"dir\":\"Books\"") && strstr(resp, "{\"name\":\"Sci\",\"mt\":") &&
              strstr(resp, "{\"name\":\"a.pdf\",\"size\":1000,\"mt\":") && strstr(resp, "{\"name\":\"b.pdf\",\"size\":1001,\"mt\":"),
          "Books: %d %s", code, resp);
    /* Запреты */
    const char *bad[] = { "/ls?dir=..", "/ls?dir=%2e%2e", "/ls?dir=Books/../..", "/ls?dir=../outside", "/ls?dir=system",
                          "/ls?dir=SYSTEM", "/ls?dir=applications", "/ls?dir=.hidden", "/ls?dir=link",
                          "/ls?dir=Wrap/hubdata", "/ls?dir=Books%5C..%5C..", "/ls?dir=%2Fetc" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        code = http_get(port, bad[i], resp, sizeof resp);
        CHECK(code == 403 || code == 404, "%s: ждали отказ, код %d", bad[i], code);
        CHECK(!strstr(resp, "secret") && !strstr(resp, "fw.bin"), "%s: утечка", bad[i]);
    }
    code = http_get(port, "/ls?dir=nope", resp, sizeof resp);
    CHECK(code == 404, "нет такой папки: %d", code);
    code = http_get(port, "/count?path=Books", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "\"files\":3"), "/count Books: %d %s", code, resp);

    /* Загрузка в папку */
    char body[3000];
    memset(body, 'q', sizeof body);
    code = http_call(port, "PUT", "/upload?name=new.pdf&dir=Books%2FSci", 0, body, 3000, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/Sci/new.pdf", lib);
    CHECK(code == 200 && fsize(p) == 3000, "загрузка в подпапку: %d", code);
    code = http_call(port, "PUT", "/upload?name=root.pdf&dir=", 0, body, 2000, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/root.pdf", lib);
    CHECK(code == 200 && fsize(p) == 2000, "загрузка в корень: %d", code);
    code = http_call(port, "PUT", "/upload?name=def.pdf", 0, body, 1500, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Downloads/def.pdf", lib);
    CHECK(code == 200 && fsize(p) == 1500, "без dir — в папку приёма: %d", code);
    code = http_call(port, "PUT", "/upload?name=%D0%BA%D0%BD%D0%B8%D0%B3%D0%B0.pdf&dir=Books", 0, body, 800, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/книга.pdf", lib);
    CHECK(code == 200 && fsize(p) == 800, "русское имя в папку: %d", code);
    const char *badup[] = { "&dir=system", "&dir=..", "&dir=Books/../../outside", "&dir=Wrap/hubdata", "&dir=link", "&dir=.hidden",
                            "&dir=applications", "&dir=%2e%2e%2foutside" };
    for (size_t i = 0; i < sizeof badup / sizeof *badup; i++) {
        char tg[200];
        snprintf(tg, sizeof tg, "/upload?name=evil.pdf%s", badup[i]);
        code = http_call(port, "PUT", tg, 0, body, 100, resp, sizeof resp);
        CHECK(code == 403 || code == 404, "%s: код %d", tg, code);
    }
    CHECK(!exists("evil") && count_part_files(lib) == 0, "лишние файлы после отказов");
    snprintf(p, sizeof p, "%s/outside/evil.pdf", base);
    CHECK(!exists(p), "файл ушёл наружу по ссылке");
    snprintf(p, sizeof p, "%s/system/evil.pdf", lib);
    CHECK(!exists(p), "файл в system");

    /* Создание папки */
    code = http_call(port, "POST", "/mkdir?dir=Books&name=New", 0, "", 0, resp, sizeof resp);
    CHECK(code == 403, "mkdir без заголовка страницы: %d", code);
    code = http_call(port, "POST", "/mkdir?dir=Books&name=New", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/New", lib);
    CHECK(code == 200 && exists(p), "mkdir: %d %s", code, resp);
    code = http_call(port, "POST", "/mkdir?dir=Books&name=New", 1, "", 0, resp, sizeof resp);
    CHECK(code == 409 && strstr(resp, "error"), "mkdir занято: %d", code);
    code = http_call(port, "POST", "/mkdir?dir=Books&name=..%2F..%2Fevil", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/evil", lib);
    CHECK(!exists(p), "mkdir вышел за папку");
    snprintf(p, sizeof p, "%s/Books/evil", lib);
    CHECK(code == 200 && exists(p), "mkdir с «..» в имени → безопасное имя: %d", code);
    code = http_call(port, "POST", "/mkdir?dir=system&name=x", 1, "", 0, resp, sizeof resp);
    CHECK(code == 403, "mkdir в system: %d", code);
    code = http_call(port, "POST", "/mkdir?dir=&name=system", 1, "", 0, resp, sizeof resp);
    CHECK(code == 409, "mkdir «system» в корне: %d", code);
    code = http_call(port, "POST", "/mkdir?dir=Books&name=", 1, "", 0, resp, sizeof resp);
    CHECK(code == 400, "mkdir без имени: %d", code);
    code = http_call(port, "GET", "/mkdir?dir=Books&name=Z", 1, "", 0, resp, sizeof resp);
    CHECK(code == 405, "mkdir GET: %d", code);
    code = http_call(port, "POST", "/mkdir?dir=Books&name=%D0%9D%D0%BE%D0%B2%D0%B0%D1%8F", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/Новая", lib);
    CHECK(code == 200 && exists(p), "mkdir по-русски: %d", code);

    /* Удаление и переименование при выключенном разрешении */
    code = http_call(port, "POST", "/delete?path=Books%2Fa.pdf", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/a.pdf", lib);
    CHECK(code == 403 && exists(p), "удаление при выключенном разрешении: %d", code);
    code = http_call(port, "POST", "/rename?path=Books%2Fa.pdf&name=z.pdf", 1, "", 0, resp, sizeof resp);
    CHECK(code == 403 && exists(p), "переименование при выключенном разрешении: %d", code);
    code = http_call(port, "POST", "/delete?path=Books", 1, "", 0, resp, sizeof resp);
    CHECK(code == 403, "удаление папки при выключенном: %d", code);

    /* Разрешили */
    long gen0;
    {
        xfer_snap sn;
        xfer_snapshot(&log, &sn);
        gen0 = sn.books_gen;
    }
    xfer_set_allow_edit(&log, 1);
    code = http_get(port, "/ls?dir=Books", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "\"edit\":true"), "/ls говорит edit:true: %s", resp);
    code = http_call(port, "POST", "/delete?path=Books%2Fa.pdf", 0, "", 0, resp, sizeof resp);
    CHECK(code == 403 && exists(p), "удаление без заголовка страницы: %d", code);
    code = http_call(port, "POST", "/delete?path=Books%2Fa.pdf", 1, "", 0, resp, sizeof resp);
    CHECK(code == 200 && !exists(p), "удаление файла: %d %s", code, resp);
    {
        xfer_snap sn;
        xfer_snapshot(&log, &sn);
        CHECK(sn.books_gen > gen0, "после удаления библиотека должна перечитаться (books_gen)");
    }
    code = http_call(port, "POST", "/delete?path=Books%2Fa.pdf", 1, "", 0, resp, sizeof resp);
    CHECK(code == 404, "удалить уже удалённое: %d", code);
    code = http_get(port, "/count?path=Books%2FSci", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "\"files\":2"), "/count Sci: %s", resp);
    code = http_call(port, "POST", "/delete?path=Books%2FSci", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/Sci", lib);
    CHECK(code == 200 && !exists(p), "удаление папки с содержимым: %d", code);
    const char *nodel[] = { "", "system", "system%2Ffw.bin", "applications", ".hidden", "link", "..",
                            "Books%2F..%2Fsystem", "Books%2F..%2F..", "Wrap", "Wrap%2Fhubdata", "%2F", "Books%2F.." };
    for (size_t i = 0; i < sizeof nodel / sizeof *nodel; i++) {
        char tg[200];
        snprintf(tg, sizeof tg, "/delete?path=%s", nodel[i]);
        code = http_call(port, "POST", tg, 1, "", 0, resp, sizeof resp);
        CHECK(code == 403 || code == 404, "%s: ждали отказ, код %d", tg, code);
    }
    snprintf(p, sizeof p, "%s/system/fw.bin", lib);
    CHECK(exists(p), "system тронут");
    snprintf(p, sizeof p, "%s/outside/secret.txt", base);
    CHECK(exists(p), "файл снаружи тронут");
    snprintf(p, sizeof p, "%s/Wrap/hubdata/z.txt", lib);
    CHECK(exists(p), "папка с данными хаба внутри удалена");
    snprintf(p, sizeof p, "%s/link", lib);
    CHECK(exists(p), "ссылка удалена");
    code = http_call(port, "POST", "/rename?path=Wrap&name=Wrap2", 1, "", 0, resp, sizeof resp);
    CHECK(code == 403, "переименование папки, где лежит папка хаба: %d", code);
    code = http_call(port, "POST", "/rename?path=link&name=l2", 1, "", 0, resp, sizeof resp);
    CHECK(code == 403, "переименование ссылки: %d", code);
    code = http_call(port, "POST", "/rename?path=system&name=s2", 1, "", 0, resp, sizeof resp);
    CHECK(code == 403, "переименование system: %d", code);

    /* Переименование */
    code = http_call(port, "POST", "/rename?path=Books%2Fb.pdf&name=c.pdf", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/c.pdf", lib);
    CHECK(code == 200 && exists(p), "переименование: %d %s", code, resp);
    snprintf(p, sizeof p, "%s/Books/b.pdf", lib);
    CHECK(!exists(p), "старое имя осталось");
    code = http_call(port, "POST", "/rename?path=Books%2Fc.pdf&name=%D0%BA%D0%BD%D0%B8%D0%B3%D0%B0.pdf", 1, "", 0, resp,
                     sizeof resp);
    CHECK(code == 409 && strstr(resp, "error"), "переименование на занятое имя: %d %s", code, resp);
    snprintf(p, sizeof p, "%s/Books/c.pdf", lib);
    CHECK(exists(p), "при 409 файл должен остаться");
    code = http_call(port, "POST", "/rename?path=Books%2Fc.pdf&name=c.pdf", 1, "", 0, resp, sizeof resp);
    CHECK(code == 200, "то же имя: %d", code);
    code = http_call(port, "POST", "/rename?path=Books%2Fc.pdf&name=..%2F..%2Fx%3Ay.pdf", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/x_y.pdf", lib);
    CHECK(code == 200 && exists(p), "имя с «..» и «:» → безопасное: %d %s", code, resp);
    snprintf(p, sizeof p, "%s/x_y.pdf", lib);
    CHECK(!exists(p), "переименование вынесло файл из папки");
    code = http_call(port, "POST", "/rename?path=Books%2Fx_y.pdf&name=", 1, "", 0, resp, sizeof resp);
    CHECK(code == 400, "пустое имя: %d", code);
    code = http_call(port, "POST", "/rename?path=Books&name=system", 1, "", 0, resp, sizeof resp);
    CHECK(code == 409, "папку в корне нельзя назвать «system»: %d", code);
    code = http_call(port, "POST", "/rename?path=Books&name=Library", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Library/x_y.pdf", lib);
    CHECK(code == 200 && exists(p), "переименование папки: %d", code);
    code = http_call(port, "POST", "/rename?path=Library&name=Books", 1, "", 0, resp, sizeof resp);
    CHECK(code == 200, "обратно: %d", code);

    /* Выключили — снова нельзя */
    xfer_set_allow_edit(&log, 0);
    code = http_call(port, "POST", "/delete?path=Books%2Fx_y.pdf", 1, "", 0, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/x_y.pdf", lib);
    CHECK(code == 403 && exists(p), "после выключения разрешения: %d", code);

    /* С паролем на приём: всё то же за Basic */
    xfer_set_password(&log, "секрет");
    xfer_set_allow_edit(&log, 1);
    g_auth[0] = 0;
    code = http_get(port, "/ls", resp, sizeof resp);
    CHECK(code == 401, "/ls без пароля: %d", code);
    code = http_call(port, "POST", "/delete?path=Books%2Fx_y.pdf", 1, "", 0, resp, sizeof resp);
    CHECK(code == 401 && exists(p), "удаление без пароля: %d", code);
    snprintf(g_auth, sizeof g_auth, "user:секрет");
    code = http_get(port, "/ls?dir=Books", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "x_y.pdf"), "/ls с паролем: %d", code);
    code = http_call(port, "PUT", "/upload?name=pw.pdf&dir=Books", 0, body, 500, resp, sizeof resp);
    snprintf(p, sizeof p, "%s/Books/pw.pdf", lib);
    CHECK(code == 200 && exists(p), "загрузка в папку с паролем: %d", code);
    code = http_call(port, "POST", "/delete?path=Books%2Fpw.pdf", 1, "", 0, resp, sizeof resp);
    CHECK(code == 200 && !exists(p), "удаление с паролем: %d", code);

    /* Страница знает про папки */
    code = http_get(port, "/", resp, sizeof resp);
    CHECK(code == 200 && strstr(resp, "/ls") && strstr(resp, "/delete") && strstr(resp, "/rename"), "страница: %d %.200s", code, resp);

    g_auth[0] = 0;
    httpd_stop(h);
    xfer_destroy(&log);

    /* Папка хаба прямо в корне (hidden_dir = lib/hub): не видна и закрыта. */
    xfer log3;
    xfer_init(&log3);
    httpd *h3 = httpd_start(&log3, dl, lib, hub, 18385);
    if (h3) {
        code = http_get(httpd_port(h3), "/ls?dir=", resp, sizeof resp);
        CHECK(code == 200 && !strstr(resp, "\"hub\"") && strstr(resp, "Books") && strstr(resp, "Wrap"), "корень без хаба: %s", resp);
        code = http_get(httpd_port(h3), "/ls?dir=hub", resp, sizeof resp);
        CHECK(code == 403 && !strstr(resp, "x.txt"), "папка хаба закрыта: %d", code);
        xfer_set_allow_edit(&log3, 1);
        code = http_call(httpd_port(h3), "POST", "/delete?path=hub", 1, "", 0, resp, sizeof resp);
        snprintf(p, sizeof p, "%s/hub/x.txt", lib);
        CHECK(code == 403 && exists(p), "папку хаба не удалить: %d", code);
        code = http_call(httpd_port(h3), "PUT", "/upload?name=h.pdf&dir=hub", 0, body, 10, resp, sizeof resp);
        CHECK(code == 403, "в папку хаба не загрузить: %d", code);
        httpd_stop(h3);
    }
    xfer_destroy(&log3);

    /* Без корня — только папка приёма; список — её же. */
    xfer log2;
    xfer_init(&log2);
    httpd *h2 = httpd_start(&log2, dl, NULL, NULL, 18390);
    if (h2) {
        code = http_get(httpd_port(h2), "/ls", resp, sizeof resp);
        CHECK(code == 200 && strstr(resp, "\"dir\":\"\"") && strstr(resp, "def.pdf"), "без корня: %d %s", code, resp);
        code = http_get(httpd_port(h2), "/ls?dir=..", resp, sizeof resp);
        CHECK(code == 403, "без корня «..»: %d", code);
        httpd_stop(h2);
    }
    xfer_destroy(&log2);
}

/* recv.txt: пароль и разрешение живут вместе и не мешают друг другу. */
static void test_edit_file(const char *base)
{
    char f[700], out[300];
    snprintf(f, sizeof f, "%s/recv-edit.txt", base);
    remove(f);
    CHECK(xfer_edit_load(f) == 0, "нет файла — запрещено");
    CHECK(xfer_edit_save(f, 1) == 0 && xfer_edit_load(f) == 1, "разрешение записалось");
    CHECK(xfer_pass_load(f, out, sizeof out) == 0, "без пароля в файле с разрешением");
    CHECK(xfer_pass_save(f, "п4ss") == 0 && xfer_edit_load(f) == 1 && xfer_pass_load(f, out, sizeof out) == 1 &&
              !strcmp(out, "п4ss"),
          "пароль не стёр разрешение");
    CHECK(xfer_edit_save(f, 0) == 0 && xfer_edit_load(f) == 0 && xfer_pass_load(f, out, sizeof out) == 1 && !strcmp(out, "п4ss"),
          "выключение разрешения не стёрло пароль");
    CHECK(xfer_edit_save(f, 1) == 0 && xfer_pass_save(f, "") == 0 && exists(f) && xfer_edit_load(f) == 1 &&
              xfer_pass_load(f, out, sizeof out) == 0,
          "убрали пароль — разрешение осталось");
    CHECK(xfer_edit_save(f, 0) == 0 && !exists(f), "нечего хранить — файл удалён");
    xfer x;
    xfer_init(&x);
    CHECK(!xfer_allow_edit(&x), "по умолчанию выключено");
    xfer_set_allow_edit(&x, 1);
    CHECK(xfer_allow_edit(&x) == 1, "включилось");
    xfer_destroy(&x);
}

/* ---- журнал и QR ---- */

static void test_xfer(void)
{
    xfer log;
    xfer_init(&log);
    int a = xfer_begin(&log, "a.epub", 1000, 0);
    xfer_progress(&log, a, 400);
    xfer_snap sn;
    xfer_snapshot(&log, &sn);
    CHECK(sn.nactive == 1 && sn.active[0].size == 400 && sn.active[0].total == 1000 && sn.ndone == 0, "идущая");
    CHECK(xfer_active_count(&log) == 1, "xfer_active_count: %d", xfer_active_count(&log));
    xfer_end(&log, a, 1, "a (2).epub", 1000);
    CHECK(xfer_active_count(&log) == 0, "xfer_active_count после конца: %d", xfer_active_count(&log));
    xfer_snapshot(&log, &sn);
    CHECK(sn.nactive == 0 && sn.ndone == 1 && !strcmp(sn.done[0].name, "a (2).epub") && sn.books_gen == 1, "принятая");
    int b = xfer_begin(&log, "b", -1, 1);
    xfer_end(&log, b, 0, NULL, 0);
    xfer_snapshot(&log, &sn);
    CHECK(sn.nactive == 0 && sn.ndone == 1 && sn.books_gen == 1, "неудавшаяся не попадает в список");
    for (int i = 0; i < XFER_DONE_MAX + 10; i++)
        xfer_end(&log, xfer_begin(&log, "x", 1, 0), 1, "x", 1);
    xfer_snapshot(&log, &sn);
    CHECK(sn.ndone == XFER_DONE_MAX && sn.total_done == XFER_DONE_MAX + 11, "вытеснение старых");
    int ids[XFER_ACTIVE_MAX + 1], over = 0;
    for (int i = 0; i <= XFER_ACTIVE_MAX; i++)
        if ((ids[i] = xfer_begin(&log, "y", 1, 0)) < 0)
            over++;
    CHECK(over == 1, "лишний приём не отклонён");
    xfer_destroy(&log);
}

static void test_qr(void)
{
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)], qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    const char *url = "http://192.168.100.100:8080";
    CHECK(qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, 8, qrcodegen_Mask_AUTO, true),
          "QR не собрался");
    int n = qrcodegen_getSize(qr);
    CHECK(n >= 21 && n <= 49 && (n - 17) % 4 == 0, "размер QR %d", n);
    /* Поисковые узоры в трёх углах: рамка 7×7, центр 3×3 тёмные, кольцо между ними светлое. */
    int corners[3][2] = { { 0, 0 }, { n - 7, 0 }, { 0, n - 7 } };
    for (int k = 0; k < 3; k++) {
        int x0 = corners[k][0], y0 = corners[k][1];
        CHECK(qrcodegen_getModule(qr, x0, y0) && qrcodegen_getModule(qr, x0 + 6, y0 + 6) &&
                  qrcodegen_getModule(qr, x0 + 3, y0 + 3) && !qrcodegen_getModule(qr, x0 + 1, y0 + 1) &&
                  !qrcodegen_getModule(qr, x0 + 5, y0 + 5),
              "поисковый узор %d", k);
    }
}

int main(void)
{
    char base[] = "/tmp/recv-test-XXXXXX";
    if (!mkdtemp(base))
        return 2;
    secret_init();
    test_xfer();
    test_qr();
    test_http(base);
    test_http_password(base);
    test_ftp(base);
    test_pass_file(base);
    test_folders(base);
    test_edit_file(base);
    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", base);
    if (!fails && system(cmd) != 0)
        printf("не удалось убрать %s\n", base);
    if (fails)
        printf("recv: упало %d проверок (файлы в %s)\n", fails, base);
    else
        printf("recv: всё хорошо\n");
    return fails ? 1 : 0;
}
