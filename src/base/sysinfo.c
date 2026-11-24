/* _DEFAULT_SOURCE: d_type, strtok_r. */
#define _DEFAULT_SOURCE
#include "sysinfo.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define MAX_PROC 256
#define SHOW 24

typedef struct {
    int pid, uid;
    char name[48];
    char state;
    long rss_kb;
    double cpu_s; /* utime + stime с запуска процесса */
} proc;

static long meminfo(const char *key)
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f)
        return -1;
    char line[128];
    long v = -1;
    size_t n = strlen(key);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, n) && line[n] == ':') {
            v = strtol(line + n + 1, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

static int read_proc(int pid, proc *p, long tick, long page_kb)
{
    char path[64], buf[1024];
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r)
        return -1;
    snprintf(p->name, sizeof p->name, "%.*s", (int)(r - l - 1), l + 1);
    /* после «) »: state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime … rss — 24-е */
    char *save = NULL, *tok = strtok_r(r + 2, " ", &save);
    unsigned long ut = 0, st = 0;
    long rss = 0;
    for (int i = 0; tok; i++, tok = strtok_r(NULL, " ", &save)) {
        if (i == 0)
            p->state = tok[0];
        else if (i == 11)
            ut = strtoul(tok, NULL, 10);
        else if (i == 12)
            st = strtoul(tok, NULL, 10);
        else if (i == 21) {
            rss = strtol(tok, NULL, 10);
            break;
        }
    }
    p->pid = pid;
    p->cpu_s = (double)(ut + st) / (double)tick;
    p->rss_kb = rss * page_kb;
    p->uid = -1;
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    f = fopen(path, "r");
    if (f) {
        while (fgets(buf, sizeof buf, f))
            if (!strncmp(buf, "Uid:", 4)) {
                p->uid = atoi(buf + 4);
                break;
            }
        fclose(f);
    }
    return 0;
}

static int by_rss(const void *a, const void *b)
{
    long x = ((const proc *)a)->rss_kb, y = ((const proc *)b)->rss_kb;
    return x < y ? 1 : x > y ? -1 : 0;
}

void sysinfo_log(const char *why)
{
    long tick = sysconf(_SC_CLK_TCK), page_kb = sysconf(_SC_PAGESIZE) / 1024;
    if (tick <= 0)
        tick = 100;
    if (page_kb <= 0)
        page_kb = 4;
    double load[3] = { 0, 0, 0 };
    FILE *f = fopen("/proc/loadavg", "r");
    if (f) {
        if (fscanf(f, "%lf %lf %lf", &load[0], &load[1], &load[2]) != 3)
            load[0] = load[1] = load[2] = 0;
        fclose(f);
    }
    long avail = meminfo("MemAvailable"); /* в старых ядрах (книга — 3.x) поля нет */
    if (avail < 0)
        avail = meminfo("MemFree") + meminfo("Buffers") + meminfo("Cached");
    log_msg("система (%s): память %ld МБ, доступно %ld МБ, загрузка %.2f %.2f %.2f", why, meminfo("MemTotal") / 1024,
            avail / 1024, load[0], load[1], load[2]);
    proc *ps = calloc(MAX_PROC, sizeof *ps);
    DIR *d = opendir("/proc");
    if (!ps || !d) {
        free(ps);
        if (d)
            closedir(d);
        return;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < MAX_PROC) {
        if (!isdigit((unsigned char)e->d_name[0]))
            continue;
        if (read_proc(atoi(e->d_name), &ps[n], tick, page_kb) == 0 && ps[n].rss_kb > 0)
            n++; /* без памяти — потоки ядра */
    }
    closedir(d);
    qsort(ps, (size_t)n, sizeof *ps, by_rss);
    log_msg("процессы (%d с памятью, первые %d по памяти): имя pid uid состояние МБ процессор_с", n, SHOW);
    for (int i = 0; i < n && i < SHOW; i++)
        log_msg("  %-20s %5d %4d %c %6.1f %8.1f", ps[i].name, ps[i].pid, ps[i].uid, ps[i].state,
                ps[i].rss_kb / 1024.0, ps[i].cpu_s);
    free(ps);
}
