#define _DEFAULT_SOURCE /* DT_DIR, realpath */
#include "dirpath.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Корень без «/» на конце ("/" остаётся пустым: путь собирается как root + "/" + rel). */
static size_t root_len(const char *root)
{
    size_t n = strlen(root);
    while (n && root[n - 1] == '/')
        n--;
    return n;
}

/* Нормализует rel в out: компоненты через «/», без пустых и «.». -1 — «..», «\», управляющие. */
static int normalize(const char *rel, char *out, size_t len)
{
    size_t o = 0;
    const char *p = rel ? rel : "";
    out[0] = 0;
    while (*p) {
        while (*p == '/')
            p++;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        size_t n = (size_t)(e - p);
        if (n == 0)
            break;
        if (n == 1 && *p == '.') {
            p = e;
            continue;
        }
        if (n == 2 && p[0] == '.' && p[1] == '.')
            return -1;
        for (const char *c = p; c < e; c++)
            if ((unsigned char)*c < 0x20 || *c == '\\')
                return -1;
        if (o + n + 2 > len)
            return -1;
        if (o)
            out[o++] = '/';
        memcpy(out + o, p, n);
        o += n;
        out[o] = 0;
        p = e;
    }
    return 0;
}

int dirpath_join(const char *root, const char *rel, char *out, size_t len)
{
    char n[1024];
    if (!root || !*root || normalize(rel, n, sizeof n) != 0)
        return -1;
    int w = *n ? snprintf(out, len, "%.*s/%s", (int)root_len(root), root, n)
               : snprintf(out, len, "%.*s", (int)root_len(root), root);
    if (w < 0 || (size_t)w >= len)
        return -1;
    if (!*out) /* корень «/» */
        snprintf(out, len, "/");
    return 0;
}

int dirpath_relative(const char *root, const char *abs, char *rel, size_t len)
{
    if (!root || !*root || !abs)
        return -1;
    size_t rl = root_len(root);
    if (strncmp(abs, root, rl) != 0 || (abs[rl] != 0 && abs[rl] != '/'))
        return -1;
    char n[1024];
    if (normalize(abs + rl, n, sizeof n) != 0 || strlen(n) >= len)
        return -1;
    strcpy(rel, n);
    return 0;
}

int dirpath_config(const char *root, const char *value, char *out, size_t len)
{
    if (!value || !*value)
        return dirpath_join(root, "Downloads", out, len);
    if (*value == '/') {
        int w = snprintf(out, len, "%s", value);
        return w < 0 || (size_t)w >= len ? -1 : 0;
    }
    return dirpath_join(root, value, out, len);
}

int dirpath_hidden(const char *name, int at_root)
{
    static const char *const names[] = { "system", "applications", "washi", "fonts", "screensavers",
                                         "System Volume Information" };
    if (name[0] == '.')
        return 1;
    if (at_root)
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            if (!strcasecmp(name, names[i]))
                return 1;
    return 0;
}

static int cmp_names(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int c = strcasecmp(x, y);
    return c ? c : strcmp(x, y);
}

int dirpath_subdirs(const char *root, const char *hide, const char *rel, char ***names, int *n)
{
    char dir[1024], nrel[1024];
    *names = NULL;
    *n = 0;
    if (dirpath_join(root, rel, dir, sizeof dir) != 0 || normalize(rel, nrel, sizeof nrel) != 0)
        return -1;
    DIR *dp = opendir(dir);
    if (!dp)
        return -1;
    char real_root[PATH_MAX];
    int have_root = 0;
    int cap = 0, count = 0, at_root = !*nrel;
    char **list = NULL;
    struct dirent *e;
    while ((e = readdir(dp)) != NULL) {
        if (dirpath_hidden(e->d_name, at_root))
            continue;
        char full[2048];
        if (snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full)
            continue;
        if (hide && *hide && !strcmp(full, hide))
            continue;
        struct stat st;
        if (e->d_type == DT_DIR) {
            /* обычная папка */
        } else if (e->d_type == DT_LNK || e->d_type == DT_UNKNOWN) {
            if (stat(full, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            /* ссылка (или неизвестно): только если ведёт внутрь библиотеки */
            if (!have_root)
                have_root = realpath(root, real_root) != NULL;
            char rp[PATH_MAX];
            size_t rl;
            if (!have_root || !realpath(full, rp))
                continue;
            rl = strlen(real_root);
            if (strncmp(rp, real_root, rl) != 0 || (rp[rl] != 0 && rp[rl] != '/' && rl > 1))
                continue;
        } else {
            continue;
        }
        if (count == cap) {
            char **nl = realloc(list, (size_t)(cap ? cap * 2 : 32) * sizeof *list);
            if (!nl)
                break;
            list = nl;
            cap = cap ? cap * 2 : 32;
        }
        char *dup = strdup(e->d_name);
        if (!dup)
            break;
        list[count++] = dup;
    }
    closedir(dp);
    if (count > 1)
        qsort(list, (size_t)count, sizeof *list, cmp_names);
    *names = list;
    *n = count;
    return 0;
}

void dirpath_free(char **names, int n)
{
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
}

int dirpath_choice_load(const char *file, char *rel, size_t len)
{
    char line[1024];
    FILE *f = fopen(file, "r");
    if (!f)
        return 0;
    char *got = fgets(line, sizeof line, f);
    fclose(f);
    if (!got)
        return 0;
    line[strcspn(line, "\r\n")] = 0;
    char n[1024];
    if (!strcmp(line, ".")) { /* сам корень */
        snprintf(rel, len, ".");
        return len > 1;
    }
    if (normalize(line, n, sizeof n) != 0 || !*n || strlen(n) >= len)
        return 0;
    strcpy(rel, n);
    return 1;
}

int dirpath_choice_save(const char *file, const char *rel)
{
    char n[1024];
    if (rel && !strcmp(rel, "."))
        snprintf(n, sizeof n, ".");
    else if (!rel || normalize(rel, n, sizeof n) != 0 || !*n)
        return unlink(file) == 0 || access(file, F_OK) != 0 ? 0 : -1;
    FILE *f = fopen(file, "w");
    if (!f)
        return -1;
    int bad = fprintf(f, "%s\n", n) < 0;
    return (fclose(f) != 0 || bad) ? -1 : 0;
}
