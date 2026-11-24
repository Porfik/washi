/* _GNU_SOURCE: MSG_NOSIGNAL, strcasestr. */
#define _GNU_SOURCE
#include "ftpd.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../base/log.h"
#include "net.h"

#define LINE_MAX_LEN 2048
#define IDLE_MS (5 * 60 * 1000)   /* клиент молчит — отключить */
#define DATA_WAIT_MS 15000        /* ждать подключения данных после PASV */
#define DATA_IDLE_MS 30000        /* данные не идут — обрыв */
#define RESERVE_BYTES (8LL << 20) /* без этого места на диске файл не принимать */
#define STACK_SIZE (256 * 1024)
#define MAX_HIDDEN 4

struct ftpd {
    xfer *log;
    char root[1024], real_root[PATH_MAX];
    char hidden[MAX_HIDDEN][PATH_MAX];
    char home[1024]; /* виртуальный путь, где клиент оказывается после входа */
    int nhidden;
    int fd, port;
    volatile int stop;
    pthread_t thread;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    int clients;
};

typedef struct {
    ftpd *f;
    int fd;
    int user_ok, logged;
    int bad;     /* неверных паролей в этом соединении */
    int pw_gen;  /* поколение пароля на входе: сменили — входить заново */
    char cwd[1024]; /* виртуальный путь: «/» — корень */
    int pasv;       /* слушающий сокет данных или -1 */
    long long rest;
} sess;

/* ---- ответы ---- */

static void say(sess *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(sess *s, const char *fmt, ...)
{
    char buf[2400];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf - 2, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n > sizeof buf - 3)
        n = sizeof buf - 3;
    buf[n++] = '\r';
    buf[n++] = '\n';
    xfer_send_all(s->fd, buf, (size_t)n, &s->f->stop);
}

/* ---- пути ---- */

/* arg (относительно cwd или с «/») → нормализованный виртуальный путь в out. -1 — выход за корень. */
static int vnorm(const char *cwd, const char *arg, char *out, size_t len)
{
    char tmp[2 * LINE_MAX_LEN];
    if (*arg == '/')
        snprintf(tmp, sizeof tmp, "%s", arg);
    else
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, arg);
    size_t o = 0;
    out[0] = 0;
    for (char *c = tmp; *c;) {
        while (*c == '/')
            c++;
        char *e = c;
        while (*e && *e != '/')
            e++;
        size_t cl = (size_t)(e - c);
        if (cl == 0) {
            break;
        } else if (cl == 1 && c[0] == '.') {
        } else if (cl == 2 && c[0] == '.' && c[1] == '.') {
            if (o == 0)
                return -1;
            while (o > 0 && out[o - 1] != '/')
                o--;
            o--; /* и сам «/» */
            out[o] = 0;
        } else {
            if (o + 1 + cl >= len)
                return -1;
            out[o++] = '/';
            memcpy(out + o, c, cl);
            o += cl;
            out[o] = 0;
        }
        c = e;
    }
    if (o == 0)
        snprintf(out, len, "/");
    return 0;
}

static void abs_of(ftpd *f, const char *virt, char *out, size_t len)
{
    if (!strcmp(virt, "/"))
        snprintf(out, len, "%s", *f->root ? f->root : "/");
    else
        snprintf(out, len, "%s%s", f->root, virt);
}

static int under(const char *path, const char *base)
{
    size_t n = strlen(base);
    return !strncasecmp(path, base, n) && (path[n] == 0 || path[n] == '/');
}

static int is_hidden(ftpd *f, const char *abs)
{
    for (int i = 0; i < f->nhidden; i++)
        if (under(abs, f->hidden[i]))
            return 1;
    return 0;
}

/* Доступен ли путь: не скрыт и (если есть на диске) настоящий путь — внутри корня.
 * Для ещё не созданного файла проверяется папка. */
static int allowed(ftpd *f, const char *abs)
{
    if (is_hidden(f, abs))
        return 0;
    char real[PATH_MAX], probe[1100];
    snprintf(probe, sizeof probe, "%s", abs);
    for (;;) {
        if (realpath(probe, real))
            break;
        char *slash = strrchr(probe, '/');
        if (errno != ENOENT || !slash || slash == probe)
            return 0;
        *slash = 0;
    }
    return under(real, f->real_root) && !is_hidden(f, real);
}

/* Аргумент команды → виртуальный и настоящий путь. 0 — можно, -1 — нельзя (ответ уже отправлен). */
static int resolve(sess *s, const char *arg, char *virt, size_t vl, char *abs, size_t al)
{
    if (vnorm(s->cwd, arg, virt, vl) != 0) {
        say(s, "550 Выше корня библиотеки подняться нельзя");
        return -1;
    }
    abs_of(s->f, virt, abs, al);
    if (!allowed(s->f, abs)) {
        say(s, "550 Эта папка закрыта");
        return -1;
    }
    return 0;
}

/* ---- данные ---- */

static void pasv_close(sess *s)
{
    if (s->pasv >= 0) {
        close(s->pasv);
        s->pasv = -1;
    }
}

/* Принять подключение данных (после PASV/EPSV). -1 — нет (ответ отправлен). */
static int data_open(sess *s)
{
    if (s->pasv < 0) {
        say(s, "425 Сначала PASV или EPSV (активный режим не поддерживается)");
        return -1;
    }
    int fd = -1;
    if (xfer_wait(s->pasv, POLLIN, DATA_WAIT_MS, &s->f->stop) == 1) {
        struct sockaddr_in a, b;
        socklen_t al = sizeof a, bl = sizeof b;
        fd = accept(s->pasv, (struct sockaddr *)&a, &al);
        /* Данные принимаются только с того же адреса, что и управление. */
        if (fd >= 0 && (getpeername(s->fd, (struct sockaddr *)&b, &bl) != 0 || a.sin_addr.s_addr != b.sin_addr.s_addr)) {
            close(fd);
            fd = -1;
        }
    }
    pasv_close(s);
    if (fd < 0)
        say(s, "425 Данные не подключились");
    return fd;
}

static void fmt_list_line(char *out, size_t len, const char *name, const struct stat *st, int mlsd)
{
    struct tm tm;
    if (mlsd) {
        gmtime_r(&st->st_mtime, &tm);
        snprintf(out, len, "type=%s;size=%lld;modify=%04d%02d%02d%02d%02d%02d; %s\r\n",
                 S_ISDIR(st->st_mode) ? "dir" : "file", (long long)st->st_size, tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, name);
        return;
    }
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    time_t now = time(NULL);
    localtime_r(&st->st_mtime, &tm);
    char when[32];
    if (st->st_mtime > now + 3600 || now - st->st_mtime > 180 * 86400)
        snprintf(when, sizeof when, "%s %2d  %4d", mon[tm.tm_mon], tm.tm_mday, tm.tm_year + 1900);
    else
        snprintf(when, sizeof when, "%s %2d %02d:%02d", mon[tm.tm_mon], tm.tm_mday, tm.tm_hour, tm.tm_min);
    snprintf(out, len, "%s 1 ftp ftp %12lld %s %s\r\n", S_ISDIR(st->st_mode) ? "drwxr-xr-x" : "-rw-r--r--",
             (long long)st->st_size, when, name);
}

enum { L_LIST, L_NLST, L_MLSD };

static void cmd_list(sess *s, const char *arg, int kind)
{
    while (*arg == '-') { /* «-a», «-la» — клиенты шлют по привычке */
        while (*arg && *arg != ' ')
            arg++;
        while (*arg == ' ')
            arg++;
    }
    char virt[1024], abs[1100];
    if (resolve(s, *arg ? arg : ".", virt, sizeof virt, abs, sizeof abs) != 0)
        return;
    struct stat st;
    if (stat(abs, &st) != 0) {
        say(s, "550 Такой папки нет");
        return;
    }
    if (kind == L_MLSD && !S_ISDIR(st.st_mode)) {
        say(s, "501 Это не папка");
        return;
    }
    int fd = data_open(s);
    if (fd < 0)
        return;
    say(s, "150 Список");
    int ok = 1;
    char line[1600];
    if (!S_ISDIR(st.st_mode)) {
        const char *base = strrchr(virt, '/');
        base = base ? base + 1 : virt;
        fmt_list_line(line, sizeof line, base, &st, 0);
        ok = xfer_send_all(fd, line, strlen(line), &s->f->stop) == 0;
    } else {
        DIR *d = opendir(abs);
        struct dirent *e;
        while (d && ok && (e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            char child[2300];
            snprintf(child, sizeof child, "%s/%s", abs, e->d_name);
            struct stat cs;
            if (is_hidden(s->f, child) || stat(child, &cs) != 0)
                continue;
            if (kind == L_NLST)
                snprintf(line, sizeof line, "%s\r\n", e->d_name);
            else
                fmt_list_line(line, sizeof line, e->d_name, &cs, kind == L_MLSD);
            ok = xfer_send_all(fd, line, strlen(line), &s->f->stop) == 0;
        }
        if (d)
            closedir(d);
    }
    close(fd);
    say(s, ok ? "226 Список передан" : "426 Передача прервана");
}

static void cmd_retr(sess *s, const char *arg)
{
    char virt[1024], abs[1100];
    if (resolve(s, arg, virt, sizeof virt, abs, sizeof abs) != 0)
        return;
    struct stat st;
    int in = open(abs, O_RDONLY);
    if (in < 0 || fstat(in, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (in >= 0)
            close(in);
        say(s, "550 Такого файла нет");
        return;
    }
    long long start = s->rest;
    s->rest = 0;
    if (start > 0 && lseek(in, (off_t)start, SEEK_SET) < 0) {
        close(in);
        say(s, "554 Не удалось продолжить с этого места");
        return;
    }
    int fd = data_open(s);
    if (fd < 0) {
        close(in);
        return;
    }
    say(s, "150 Отдаю файл (%lld байт)", (long long)st.st_size - start);
    char *buf = malloc(65536);
    int ok = buf != NULL;
    long long sent = 0;
    while (ok) {
        ssize_t n = read(in, buf, 65536);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        ok = xfer_send_all(fd, buf, (size_t)n, &s->f->stop) == 0;
        sent += n;
        if ((sent & 0xFFFFF) < 65536)
            xfer_touch(s->f->log);
    }
    free(buf);
    close(in);
    close(fd);
    say(s, ok ? "226 Файл отдан" : "426 Передача прервана");
}

static void cmd_stor(sess *s, const char *arg)
{
    ftpd *f = s->f;
    char virt[1024], abs[1100];
    if (resolve(s, arg, virt, sizeof virt, abs, sizeof abs) != 0)
        return;
    if (s->rest > 0) {
        s->rest = 0;
        say(s, "554 Докачка на устройство выключена");
        return;
    }
    char *slash = strrchr(abs, '/');
    char dir[1100], safe[256];
    snprintf(dir, sizeof dir, "%.*s", (int)(slash - abs), abs);
    net_safe_name(slash + 1, safe, sizeof safe);
    struct stat st;
    if (stat(dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        say(s, "550 Такой папки нет");
        return;
    }
    char path[1400];
    snprintf(path, sizeof path, "%s/%s", dir, safe);
    if (stat(path, &st) == 0 || !allowed(f, path)) {
        say(s, "550 Такой файл уже есть (перезапись выключена)");
        return;
    }
    long long fr = xfer_free_space(dir);
    if (fr >= 0 && fr < RESERVE_BYTES) {
        say(s, "552 На устройстве не хватает места");
        return;
    }
    char tmp[1500];
    int out = xfer_tmp_open(dir, tmp, sizeof tmp);
    if (out < 0) {
        say(s, "550 Не удалось создать файл");
        return;
    }
    int fd = data_open(s);
    if (fd < 0) {
        close(out);
        unlink(tmp);
        return;
    }
    int id = xfer_begin(f->log, safe, -1, 1);
    say(s, "150 Принимаю файл");
    char *buf = malloc(65536);
    long long got = 0, reported = 0;
    const char *err = buf ? NULL : "451 Не хватило памяти";
    while (!err) {
        int w = xfer_wait(fd, POLLIN, DATA_IDLE_MS, &f->stop);
        if (w <= 0) {
            err = w < 0 ? "426 Приём остановлен" : "426 Передача зависла";
            break;
        }
        ssize_t n = recv(fd, buf, 65536, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n < 0) {
            err = "426 Связь оборвалась";
            break;
        }
        if (n == 0)
            break;
        for (ssize_t off = 0; off < n;) {
            ssize_t wr = write(out, buf + off, (size_t)(n - off));
            if (wr < 0 && errno == EINTR)
                continue;
            if (wr <= 0) {
                err = errno == ENOSPC ? "552 На устройстве кончилось место" : "451 Не удалось записать файл";
                break;
            }
            off += wr;
        }
        got += n;
        if (id >= 0 && got - reported >= 256 * 1024) {
            xfer_progress(f->log, id, got);
            reported = got;
        }
    }
    free(buf);
    close(fd);
    if (!err && fsync(out) != 0)
        err = "451 Не удалось записать файл";
    close(out);
    if (!err && got == 0)
        err = "550 Файл пустой";
    char final[1500];
    if (!err && xfer_commit(tmp, dir, safe, 0, final, sizeof final) != 0)
        err = errno == EEXIST ? "550 Такой файл уже есть (перезапись выключена)"
                              : "451 Не удалось сохранить файл";
    if (err) {
        unlink(tmp);
        if (id >= 0)
            xfer_end(f->log, id, 0, NULL, 0);
        log_msg("ftp: %s не принят (%lld байт): %s", safe, got, err);
        say(s, "%s", err);
        return;
    }
    if (id >= 0)
        xfer_end(f->log, id, 1, safe, got);
    log_msg("ftp: принят %s (%lld байт)", final, got);
    say(s, "226 Файл принят");
}

static void cmd_pasv(sess *s, int extended)
{
    pasv_close(s);
    int port = 0;
    s->pasv = xfer_listen(0, 1, &port);
    if (s->pasv < 0) {
        say(s, "425 Не удалось открыть порт для данных");
        return;
    }
    if (extended) {
        say(s, "229 Перехожу в расширенный пассивный режим (|||%d|)", port);
        return;
    }
    struct sockaddr_in sa;
    socklen_t sl = sizeof sa;
    unsigned ip = 0;
    if (getsockname(s->fd, (struct sockaddr *)&sa, &sl) == 0)
        ip = ntohl(sa.sin_addr.s_addr);
    say(s, "227 Перехожу в пассивный режим (%u,%u,%u,%u,%d,%d)", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255,
        ip & 255, port >> 8, port & 255);
}

static void cmd_mkd(sess *s, const char *arg)
{
    char virt[1024], abs[1100];
    if (vnorm(s->cwd, arg, virt, sizeof virt) != 0 || !strcmp(virt, "/")) {
        say(s, "550 Выше корня библиотеки подняться нельзя");
        return;
    }
    /* Имя папки — как для файла на FAT32. */
    char *slash = strrchr(virt, '/');
    char safe[256];
    net_safe_name(slash + 1, safe, sizeof safe);
    snprintf(slash + 1, sizeof virt - (size_t)(slash + 1 - virt), "%s", safe);
    abs_of(s->f, virt, abs, sizeof abs);
    char *as = strrchr(abs, '/');
    *as = 0;
    struct stat st;
    if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode) || !allowed(s->f, abs)) {
        say(s, "550 Такой папки нет");
        return;
    }
    *as = '/';
    if (!allowed(s->f, abs)) {
        say(s, "550 Эта папка закрыта");
        return;
    }
    if (mkdir(abs, 0755) != 0) {
        say(s, errno == EEXIST ? "550 Такая папка уже есть" : "550 Не удалось создать папку");
        return;
    }
    char q[2200];
    size_t o = 0;
    for (const char *c = virt; *c && o + 2 < sizeof q; c++) {
        if (*c == '"')
            q[o++] = '"';
        q[o++] = *c;
    }
    q[o] = 0;
    say(s, "257 \"%s\" создана", q);
}

static void cmd_stat_file(sess *s, const char *arg, int what)
{
    char virt[1024], abs[1100];
    if (resolve(s, arg, virt, sizeof virt, abs, sizeof abs) != 0)
        return;
    struct stat st;
    if (stat(abs, &st) != 0) {
        say(s, "550 Такого файла нет");
        return;
    }
    if (what == 0) { /* SIZE */
        if (!S_ISREG(st.st_mode))
            say(s, "550 Это не файл");
        else
            say(s, "213 %lld", (long long)st.st_size);
    } else if (what == 1) { /* MDTM */
        struct tm tm;
        gmtime_r(&st.st_mtime, &tm);
        say(s, "213 %04d%02d%02d%02d%02d%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
            tm.tm_min, tm.tm_sec);
    } else { /* MLST */
        char line[1600], out[1800];
        fmt_list_line(line, sizeof line, virt, &st, 1);
        line[strlen(line) - 2] = 0;
        snprintf(out, sizeof out, "250-Сведения\r\n %s\r\n250 Конец\r\n", line);
        xfer_send_all(s->fd, out, strlen(out), &s->f->stop);
    }
}

/* ---- сеанс ---- */

/* Одна команда. 1 — закрыть соединение. */
static int command(sess *s, char *line)
{
    ftpd *f = s->f;
    while ((unsigned char)*line == 0xFF) { /* telnet-управление перед командой (ABOR) */
        unsigned char c = (unsigned char)line[1];
        if (!c) {
            line++;
            break;
        }
        line += c >= 251 && c <= 254 && line[2] ? 3 : 2;
    }
    char *arg = strchr(line, ' ');
    if (arg)
        *arg++ = 0;
    else
        arg = line + strlen(line);
    for (char *c = line; *c; c++)
        if (*c >= 'a' && *c <= 'z')
            *c -= 32;
    xfer_touch(f->log);
    const char *cmd = line;
#define IS(n) (!strcmp(cmd, n))

    if (IS("QUIT")) {
        say(s, "221 До свидания");
        return 1;
    }
    if (IS("USER")) {
        s->user_ok = 1;
        s->logged = 0;
        say(s, xfer_has_password(f->log) ? "331 Нужен пароль" : "331 Пароль — любой");
        return 0;
    }
    if (IS("PASS")) {
        if (!s->user_ok) {
            say(s, "503 Сначала USER");
        } else {
            s->pw_gen = xfer_password_gen(f->log);
            if (xfer_check_password(f->log, arg)) {
                s->logged = 1;
                say(s, "230 Вход выполнен");
            } else {
                s->logged = 0;
                log_msg("приём: неверный пароль (FTP)");
                xfer_auth_pause(&f->stop);
                if (++s->bad >= 3) {
                    say(s, "530 Неверный пароль три раза — соединение закрыто");
                    return 1;
                }
                say(s, "530 Неверный пароль");
            }
        }
        return 0;
    }
    if (IS("SYST")) {
        say(s, "215 UNIX Type: L8");
        return 0;
    }
    if (IS("FEAT")) {
        static const char feat[] = "211-Возможности\r\n UTF8\r\n SIZE\r\n MDTM\r\n EPSV\r\n REST STREAM\r\n"
                                   " MLST type*;size*;modify*;\r\n211 Конец\r\n";
        xfer_send_all(s->fd, feat, sizeof feat - 1, &f->stop);
        return 0;
    }
    if (IS("OPTS")) {
        say(s, "200 Хорошо");
        return 0;
    }
    if (IS("NOOP")) {
        say(s, "200 Хорошо");
        return 0;
    }
    if (IS("AUTH") || IS("PBSZ") || IS("PROT")) {
        say(s, "502 Шифрования нет");
        return 0;
    }
    if (s->logged && s->pw_gen != xfer_password_gen(f->log)) { /* пароль сменили — войти заново */
        s->logged = 0;
        s->user_ok = 0;
    }
    if (!s->logged) {
        say(s, "530 Сначала войдите (USER и PASS)");
        return 0;
    }
    if (IS("PWD") || IS("XPWD")) {
        char q[2200];
        size_t o = 0;
        for (const char *c = s->cwd; *c && o + 2 < sizeof q; c++) {
            if (*c == '"')
                q[o++] = '"';
            q[o++] = *c;
        }
        q[o] = 0;
        say(s, "257 \"%s\" — текущая папка", q);
    } else if (IS("CWD") || IS("XCWD") || IS("CDUP") || IS("XCUP")) {
        const char *to = IS("CDUP") || IS("XCUP") ? ".." : arg;
        char virt[1024], abs[1100];
        struct stat st;
        if (resolve(s, to, virt, sizeof virt, abs, sizeof abs) != 0)
            return 0;
        if (stat(abs, &st) != 0 || !S_ISDIR(st.st_mode)) {
            say(s, "550 Такой папки нет");
        } else {
            snprintf(s->cwd, sizeof s->cwd, "%s", virt);
            say(s, "250 Перешёл");
        }
    } else if (IS("TYPE")) {
        say(s, "200 Хорошо");
    } else if (IS("MODE") || IS("STRU")) {
        if (!strcasecmp(arg, "S") || !strcasecmp(arg, "F"))
            say(s, "200 Хорошо");
        else
            say(s, "504 Не поддерживается");
    } else if (IS("PASV")) {
        cmd_pasv(s, 0);
    } else if (IS("EPSV")) {
        if (!strcasecmp(arg, "ALL"))
            say(s, "200 Хорошо");
        else
            cmd_pasv(s, 1);
    } else if (IS("PORT") || IS("EPRT")) {
        say(s, "502 Только пассивный режим (PASV, EPSV)");
    } else if (IS("LIST")) {
        cmd_list(s, arg, L_LIST);
    } else if (IS("NLST")) {
        cmd_list(s, arg, L_NLST);
    } else if (IS("MLSD")) {
        cmd_list(s, arg, L_MLSD);
    } else if (IS("MLST")) {
        cmd_stat_file(s, *arg ? arg : ".", 2);
    } else if (IS("RETR")) {
        cmd_retr(s, arg);
    } else if (IS("STOR")) {
        cmd_stor(s, arg);
    } else if (IS("APPE")) {
        say(s, "550 Дописывать в файлы на устройстве нельзя");
    } else if (IS("REST")) {
        char *end;
        long long v = strtoll(arg, &end, 10);
        if (end == arg || v < 0) {
            say(s, "501 Нужно число");
        } else {
            s->rest = v;
            say(s, "350 Продолжу с байта %lld", v);
        }
    } else if (IS("SIZE")) {
        cmd_stat_file(s, arg, 0);
    } else if (IS("MDTM")) {
        cmd_stat_file(s, arg, 1);
    } else if (IS("MKD") || IS("XMKD")) {
        cmd_mkd(s, arg);
    } else if (IS("DELE") || IS("RMD") || IS("XRMD")) {
        say(s, "550 Удаление с устройства выключено");
    } else if (IS("RNFR") || IS("RNTO")) {
        say(s, "550 Переименование на устройстве выключено");
    } else if (IS("ALLO")) {
        say(s, "202 Не нужно");
    } else if (IS("SITE")) {
        say(s, "502 Не поддерживается");
    } else if (IS("ABOR")) {
        say(s, "226 Нечего прерывать");
    } else if (IS("STAT")) {
        say(s, "211 Приём книг по Wi-Fi");
    } else {
        say(s, "502 Команда не поддерживается");
    }
#undef IS
    return 0;
}

typedef struct {
    ftpd *f;
    int fd;
} fconn;

static void session(ftpd *f, int fd)
{
    sess *s = calloc(1, sizeof *s);
    char *buf = malloc(LINE_MAX_LEN + 1);
    if (!s || !buf) {
        free(s);
        free(buf);
        return;
    }
    s->f = f;
    s->fd = fd;
    s->pasv = -1;
    snprintf(s->cwd, sizeof s->cwd, "%s", f->home);
    say(s, "220 Приём книг по Wi-Fi: войдите под любым именем");
    size_t n = 0;
    int idle = 0, done = 0;
    while (!done && !f->stop) {
        int w = xfer_wait(fd, POLLIN, 1000, &f->stop);
        if (w < 0)
            break;
        if (w == 0) {
            idle += 1000;
            if (idle >= IDLE_MS)
                break;
            continue;
        }
        idle = 0;
        ssize_t r = recv(fd, buf + n, LINE_MAX_LEN - n, 0);
        if (r < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (r <= 0)
            break;
        n += (size_t)r;
        buf[n] = 0;
        char *nl;
        while (!done && (nl = memchr(buf, '\n', n))) {
            size_t ll = (size_t)(nl - buf);
            buf[ll] = 0;
            if (ll && buf[ll - 1] == '\r')
                buf[ll - 1] = 0;
            done = command(s, buf);
            memmove(buf, nl + 1, n - ll - 1);
            n -= ll + 1;
            buf[n] = 0;
        }
        if (n >= LINE_MAX_LEN) { /* строка без конца: сбросить */
            say(s, "500 Слишком длинная команда");
            n = 0;
        }
    }
    pasv_close(s);
    free(buf);
    free(s);
}

static void *client_thread(void *arg)
{
    fconn *c = arg;
    ftpd *f = c->f;
    session(f, c->fd);
    close(c->fd);
    free(c);
    pthread_mutex_lock(&f->mu);
    f->clients--;
    pthread_cond_broadcast(&f->cv);
    pthread_mutex_unlock(&f->mu);
    return NULL;
}

static void refuse(int fd)
{
    static const char msg[] = "421 Слишком много подключений, повторите позже\r\n";
    send(fd, msg, sizeof msg - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
    close(fd);
}

static void *accept_thread(void *arg)
{
    ftpd *f = arg;
    while (!f->stop) {
        if (xfer_wait(f->fd, POLLIN, 200, &f->stop) != 1)
            continue;
        int fd = accept(f->fd, NULL, NULL);
        if (fd < 0)
            continue;
        pthread_mutex_lock(&f->mu);
        int busy = f->clients >= FTPD_MAX_CLIENTS;
        if (!busy)
            f->clients++;
        pthread_mutex_unlock(&f->mu);
        fconn *c = busy ? NULL : malloc(sizeof *c);
        if (!c) {
            if (!busy) {
                pthread_mutex_lock(&f->mu);
                f->clients--;
                pthread_mutex_unlock(&f->mu);
            }
            refuse(fd);
            continue;
        }
        c->f = f;
        c->fd = fd;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setstacksize(&at, STACK_SIZE);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_t t;
        if (pthread_create(&t, &at, client_thread, c) != 0) {
            pthread_mutex_lock(&f->mu);
            f->clients--;
            pthread_mutex_unlock(&f->mu);
            free(c);
            refuse(fd);
        }
        pthread_attr_destroy(&at);
    }
    return NULL;
}

static void add_hidden(ftpd *f, const char *path)
{
    char real[PATH_MAX];
    if (f->nhidden < MAX_HIDDEN)
        snprintf(f->hidden[f->nhidden++], PATH_MAX, "%s", path);
    if (realpath(path, real) && strcmp(real, path) && f->nhidden < MAX_HIDDEN)
        snprintf(f->hidden[f->nhidden++], PATH_MAX, "%s", real);
}

/* Папка home (если она есть, внутри корня и не скрыта) → виртуальный путь f->home; иначе «/». */
static void set_home(ftpd *f, const char *home)
{
    snprintf(f->home, sizeof f->home, "/");
    char real[PATH_MAX];
    if (!home || !*home || !realpath(home, real) || !under(real, f->real_root))
        return;
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode) || is_hidden(f, real))
        return;
    size_t rl = strlen(f->real_root);
    if (!strcmp(f->real_root, "/"))
        rl = 0;
    if (real[rl] == 0)
        return; /* сам корень */
    snprintf(f->home, sizeof f->home, "%s", real + rl);
}

ftpd *ftpd_start(xfer *log, const char *root, const char *hidden_dir, const char *home, int first_port)
{
    ftpd *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->log = log;
    snprintf(f->root, sizeof f->root, "%s", root);
    size_t rl = strlen(f->root);
    while (rl > 1 && f->root[rl - 1] == '/')
        f->root[--rl] = 0;
    if (!strcmp(f->root, "/"))
        f->root[0] = 0; /* корень файловой системы: «/x» → «/x» */
    if (!realpath(*f->root ? f->root : "/", f->real_root)) {
        log_msg("ftpd: нет папки библиотеки %s", root);
        free(f);
        return NULL;
    }
    char p[1100];
    snprintf(p, sizeof p, "%s/system", f->root);
    add_hidden(f, p);
    snprintf(p, sizeof p, "%s/applications", f->root);
    add_hidden(f, p);
    if (hidden_dir && *hidden_dir) {
        snprintf(p, sizeof p, "%s", hidden_dir);
        size_t hl = strlen(p);
        while (hl > 1 && p[hl - 1] == '/')
            p[--hl] = 0;
        add_hidden(f, p);
    }
    set_home(f, home);
    pthread_mutex_init(&f->mu, NULL);
    pthread_cond_init(&f->cv, NULL);
    f->fd = xfer_listen(first_port, 10, &f->port);
    if (f->fd < 0) {
        log_msg("ftpd: нет свободного порта с %d", first_port);
        free(f);
        return NULL;
    }
    if (pthread_create(&f->thread, NULL, accept_thread, f) != 0) {
        close(f->fd);
        free(f);
        return NULL;
    }
    log_msg("ftpd: порт %d, корень %s, начало %s", f->port, f->root, f->home);
    return f;
}

int ftpd_port(const ftpd *f)
{
    return f->port;
}

const char *ftpd_home(const ftpd *f)
{
    return f->home;
}

void ftpd_stop(ftpd *f)
{
    if (!f)
        return;
    f->stop = 1;
    pthread_join(f->thread, NULL);
    close(f->fd);
    pthread_mutex_lock(&f->mu);
    while (f->clients > 0)
        pthread_cond_wait(&f->cv, &f->mu);
    pthread_mutex_unlock(&f->mu);
    pthread_mutex_destroy(&f->mu);
    pthread_cond_destroy(&f->cv);
    log_msg("ftpd: остановлен");
    free(f);
}
