/* _DEFAULT_SOURCE: dirent, MSG_NOSIGNAL. */
#define _DEFAULT_SOURCE
#include "xfer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../base/clock.h"
#include "../base/config.h"
#include "../base/log.h"
#include "../base/secret.h"
#include "../base/util.h"
#include "net.h"

void xfer_init(xfer *x)
{
    memset(x, 0, sizeof *x);
    pthread_mutex_init(&x->mu, NULL);
    x->next_id = 1;
    x->last_activity_ms = clock_ms();
}

void xfer_destroy(xfer *x)
{
    pthread_mutex_destroy(&x->mu);
}

void xfer_touch(xfer *x)
{
    pthread_mutex_lock(&x->mu);
    x->last_activity_ms = clock_ms();
    pthread_mutex_unlock(&x->mu);
}

/* ---- пароль ---- */

void xfer_set_password(xfer *x, const char *plain)
{
    pthread_mutex_lock(&x->mu);
    memset(x->pass, 0, sizeof x->pass); /* хвост прежнего пароля не должен остаться */
    snprintf(x->pass, sizeof x->pass, "%s", plain ? plain : "");
    x->pass_gen++;
    pthread_mutex_unlock(&x->mu);
}

int xfer_has_password(xfer *x)
{
    pthread_mutex_lock(&x->mu);
    int r = x->pass[0] != 0;
    pthread_mutex_unlock(&x->mu);
    return r;
}

int xfer_password_gen(xfer *x)
{
    pthread_mutex_lock(&x->mu);
    int r = x->pass_gen;
    pthread_mutex_unlock(&x->mu);
    return r;
}

int xfer_check_password(xfer *x, const char *given)
{
    char want[sizeof x->pass];
    pthread_mutex_lock(&x->mu);
    memcpy(want, x->pass, sizeof want);
    pthread_mutex_unlock(&x->mu);
    if (!want[0])
        return 1;
    size_t gl = given ? strlen(given) : 0, wl = strlen(want);
    /* Без выходов по содержимому: весь буфер, различие копится в d. */
    unsigned d = (unsigned)(gl != wl) | (given ? 0u : 1u);
    for (size_t i = 0; i < sizeof want; i++) {
        unsigned char a = given && i < gl ? (unsigned char)given[i] : 0, b = (unsigned char)want[i];
        d |= (unsigned)(a ^ b);
    }
    memset(want, 0, sizeof want);
    return d == 0;
}

void xfer_auth_pause(volatile int *stop)
{
    for (int i = 0; i < 10 && !(stop && *stop); i++)
        usleep(100000);
}

int xfer_pass_load(const char *file, char *out, size_t len)
{
    out[0] = 0;
    config c;
    if (config_load(&c, file) != 0)
        return 0;
    const char *v = config_get(&c, "password", "");
    if (!*v)
        return 0;
    if (secret_open(v, out, len) != 0 || !*out) {
        /* Пароль был, но не открылся: не открывать приём, а закрыть случайным паролем. */
        log_msg("приём: пароль не расшифровался — приём закрыт, нужно задать заново");
        unsigned char rnd[16] = { 0 };
        FILE *u = fopen("/dev/urandom", "rb");
        if (!u || fread(rnd, 1, sizeof rnd, u) != sizeof rnd)
            for (size_t i = 0; i < sizeof rnd; i++)
                rnd[i] = (unsigned char)(rand() ^ (int)clock_ms());
        if (u)
            fclose(u);
        size_t n = 0;
        for (size_t i = 0; i < sizeof rnd && n + 3 <= len; i++)
            n += (size_t)snprintf(out + n, len - n, "%02x", rnd[i]);
        return -1;
    }
    return 1;
}

/* Записать recv.txt: пароль (уже зашифрованный; пусто — нет) и разрешение. Нечего хранить — файл удаляется. */
static int recv_file_write(const char *file, const char *sealed, int edit)
{
    if (!*sealed && !edit)
        return remove(file) == 0 || errno == ENOENT ? 0 : -1;
    FILE *f = file_save_begin(file);
    if (!f)
        return -1;
    fprintf(f, "# Приём по Wi-Fi: пароль (зашифрован ключом устройства), разрешение удалять из браузера.\n");
    if (*sealed)
        fprintf(f, "password=%s\n", sealed);
    if (edit)
        fprintf(f, "edit=1\n");
    return file_save_end(f, file, 1);
}

int xfer_pass_save(const char *file, const char *plain)
{
    int edit = xfer_edit_load(file);
    char sealed[512] = ""; /* secret_sealed_len(255) — 389 */
    if (plain && *plain && secret_seal(plain, sealed, sizeof sealed) != 0)
        return -1;
    return recv_file_write(file, sealed, edit);
}

int xfer_edit_load(const char *file)
{
    config c;
    if (config_load(&c, file) != 0)
        return 0;
    return config_get_int(&c, "edit", 0) == 1;
}

int xfer_edit_save(const char *file, int on)
{
    config c;
    char sealed[512] = "";
    if (config_load(&c, file) == 0)
        snprintf(sealed, sizeof sealed, "%s", config_get(&c, "password", ""));
    return recv_file_write(file, sealed, on != 0);
}

void xfer_set_allow_edit(xfer *x, int on)
{
    pthread_mutex_lock(&x->mu);
    x->allow_edit = on != 0;
    pthread_mutex_unlock(&x->mu);
}

int xfer_allow_edit(xfer *x)
{
    pthread_mutex_lock(&x->mu);
    int r = x->allow_edit;
    pthread_mutex_unlock(&x->mu);
    return r;
}

void xfer_books_changed(xfer *x)
{
    pthread_mutex_lock(&x->mu);
    x->books_gen++;
    x->last_activity_ms = clock_ms();
    pthread_mutex_unlock(&x->mu);
}

int xfer_begin(xfer *x, const char *name, long long total, int via)
{
    int id = -1;
    pthread_mutex_lock(&x->mu);
    if (x->nactive < XFER_ACTIVE_MAX) {
        xfer_item *it = &x->active[x->nactive++];
        memset(it, 0, sizeof *it);
        snprintf(it->name, sizeof it->name, "%s", name);
        it->total = total;
        it->via = via;
        it->id = id = x->next_id++;
        x->gen++;
        x->last_activity_ms = clock_ms();
    }
    pthread_mutex_unlock(&x->mu);
    return id;
}

static int find_active(xfer *x, int id)
{
    for (int i = 0; i < x->nactive; i++)
        if (x->active[i].id == id)
            return i;
    return -1;
}

void xfer_progress(xfer *x, int id, long long done)
{
    pthread_mutex_lock(&x->mu);
    int i = find_active(x, id);
    if (i >= 0) {
        x->active[i].size = done;
        x->gen++;
        x->last_activity_ms = clock_ms();
    }
    pthread_mutex_unlock(&x->mu);
}

void xfer_end(xfer *x, int id, int ok, const char *final_name, long long size)
{
    pthread_mutex_lock(&x->mu);
    int i = find_active(x, id);
    if (i >= 0) {
        xfer_item it = x->active[i];
        memmove(&x->active[i], &x->active[i + 1], (size_t)(x->nactive - i - 1) * sizeof it);
        x->nactive--;
        if (ok) {
            memmove(&x->done[1], &x->done[0], (size_t)(XFER_DONE_MAX - 1) * sizeof it);
            snprintf(it.name, sizeof it.name, "%s", final_name);
            it.size = size;
            it.total = size;
            x->done[0] = it;
            if (x->ndone < XFER_DONE_MAX)
                x->ndone++;
            x->total_done++;
            x->books_gen++;
        }
        x->gen++;
        x->last_activity_ms = clock_ms();
    }
    pthread_mutex_unlock(&x->mu);
}

int xfer_active_count(xfer *x)
{
    pthread_mutex_lock(&x->mu);
    int n = x->nactive;
    pthread_mutex_unlock(&x->mu);
    return n;
}

void xfer_snapshot(xfer *x, xfer_snap *out)
{
    pthread_mutex_lock(&x->mu);
    memcpy(out->done, x->done, sizeof out->done);
    out->ndone = x->ndone;
    memcpy(out->active, x->active, sizeof out->active);
    out->nactive = x->nactive;
    out->gen = x->gen;
    out->books_gen = x->books_gen;
    out->total_done = x->total_done;
    out->last_activity_ms = x->last_activity_ms;
    pthread_mutex_unlock(&x->mu);
}

/* ---- сокеты ---- */

int xfer_listen(int first, int tries, int *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    for (int p = first; p < first + tries; p++) {
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = htons((uint16_t)p);
        if (bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && listen(fd, 8) == 0) {
            if (port) {
                socklen_t sl = sizeof sa;
                *port = p;
                if (p == 0 && getsockname(fd, (struct sockaddr *)&sa, &sl) == 0)
                    *port = ntohs(sa.sin_port);
            }
            return fd;
        }
        if (first == 0)
            break;
    }
    close(fd);
    return -1;
}

int xfer_wait(int fd, int events, int ms, volatile int *stop)
{
    while (ms > 0) {
        if (stop && *stop)
            return -1;
        int step = ms < 200 ? ms : 200;
        struct pollfd p = { fd, (short)events, 0 };
        int r = poll(&p, 1, step);
        if (r > 0)
            return 1;
        if (r < 0 && errno != EINTR)
            return -1;
        ms -= step;
    }
    return stop && *stop ? -1 : 0;
}

int xfer_send_all(int fd, const void *buf, size_t len, volatile int *stop)
{
    const char *p = buf;
    while (len > 0) {
        if (xfer_wait(fd, POLLOUT, 30000, stop) != 1)
            return -1;
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EAGAIN || errno == EINTR)
                continue;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* ---- файлы ---- */

long long xfer_free_space(const char *path)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (;;) {
        struct statvfs sv;
        if (statvfs(p, &sv) == 0)
            return (long long)sv.f_bavail * (long long)sv.f_frsize;
        char *slash = strrchr(p, '/');
        if (errno != ENOENT || !slash || slash == p)
            return -1;
        *slash = 0;
    }
}

static pthread_mutex_t file_mu = PTHREAD_MUTEX_INITIALIZER;
static int tmp_counter;

int xfer_tmp_open(const char *dir, char *path, size_t len)
{
    pthread_mutex_lock(&file_mu);
    int n = ++tmp_counter;
    pthread_mutex_unlock(&file_mu);
    snprintf(path, len, "%s/.recv-%d-%d.part", dir, (int)getpid(), n);
    return open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
}

void xfer_tmp_clean(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, ".recv-", 6) || n < 11 || strcmp(e->d_name + n - 5, ".part"))
            continue;
        char p[1024];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
}

int xfer_commit(const char *tmp, const char *dir, const char *name, int unique, char *out, size_t len)
{
    int r = -1;
    pthread_mutex_lock(&file_mu);
    if (unique) {
        if (net_unique_path(dir, name, out, len) == 0)
            r = rename(tmp, out);
    } else {
        char safe[256];
        struct stat st;
        net_safe_name(name, safe, sizeof safe);
        snprintf(out, len, "%s/%s", dir, safe);
        if (stat(out, &st) == 0)
            errno = EEXIST;
        else
            r = rename(tmp, out);
    }
    pthread_mutex_unlock(&file_mu);
    return r;
}
