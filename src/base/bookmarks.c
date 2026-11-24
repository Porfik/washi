#include "bookmarks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "util.h"

static void parse(bookmarks *b, const char *list)
{
    b->count = 0;
    for (const char *p = list; *p && b->count < BOOKMARKS_MAX;) {
        int v = atoi(p);
        if (v > 0)
            b->pages[b->count++] = v - 1;
        p = strchr(p, ',');
        if (!p)
            break;
        p++;
    }
}

void bookmarks_load(bookmarks *b, const char *file, const char *path)
{
    memset(b, 0, sizeof *b);
    snprintf(b->file, sizeof b->file, "%s", file);
    snprintf(b->path, sizeof b->path, "%s", path);
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *tab = strchr(line, '\t');
        if (tab && !strcmp(tab + 1, path)) {
            *tab = 0;
            parse(b, line);
            break;
        }
    }
    fclose(f);
}

int bookmarks_has(const bookmarks *b, int page)
{
    for (int i = 0; i < b->count; i++)
        if (b->pages[i] == page)
            return 1;
    return 0;
}

/* Переписать файл: строки других книг как есть, строка этой книги — заново (или без неё). */
static void save(const bookmarks *b)
{
    FILE *out = file_save_begin(b->file);
    if (!out) {
        log_msg("закладки: не записать %s", b->file);
        return;
    }
    FILE *in = fopen(b->file, "r");
    char line[4096];
    while (in && fgets(line, sizeof line, in)) {
        char *tab = strchr(line, '\t');
        char path[1100];
        snprintf(path, sizeof path, "%s", tab ? tab + 1 : "");
        path[strcspn(path, "\r\n")] = 0;
        if (tab && strcmp(path, b->path))
            fputs(line, out);
    }
    if (in)
        fclose(in);
    if (b->count) {
        for (int i = 0; i < b->count; i++)
            fprintf(out, "%s%d", i ? "," : "", b->pages[i] + 1);
        fprintf(out, "\t%s\n", b->path);
    }
    file_save_end(out, b->file, 1);
}

int bookmarks_toggle(bookmarks *b, int page)
{
    int i = 0;
    while (i < b->count && b->pages[i] < page)
        i++;
    int on;
    if (i < b->count && b->pages[i] == page) {
        memmove(&b->pages[i], &b->pages[i + 1], sizeof *b->pages * (size_t)(b->count - i - 1));
        b->count--;
        on = 0;
    } else if (b->count < BOOKMARKS_MAX) {
        memmove(&b->pages[i + 1], &b->pages[i], sizeof *b->pages * (size_t)(b->count - i));
        b->pages[i] = page;
        b->count++;
        on = 1;
    } else {
        return 1;
    }
    save(b);
    return on;
}

int bookmarks_move(const char *file, const char *from, const char *to, int folder)
{
    FILE *in = fopen(file, "r");
    if (!in)
        return 0;
    FILE *out = file_save_begin(file);
    if (!out) {
        fclose(in);
        log_msg("закладки: не записать %s", file);
        return -1;
    }
    size_t fn = strlen(from);
    char line[4096];
    int changed = 0;
    while (fgets(line, sizeof line, in)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        char *path = tab + 1;
        path[strcspn(path, "\r\n")] = 0;
        int hit = folder ? !strncmp(path, from, fn) && path[fn] == '/' : !strcmp(path, from);
        if (!folder && to && !hit && !strcmp(path, to))
            continue; /* остаток от прежней книги с таким именем */
        if (!hit) {
            fprintf(out, "%s\n", line);
            continue;
        }
        changed = 1;
        if (!to)
            continue;
        *tab = 0;
        fprintf(out, "%s\t%s%s\n", line, to, folder ? path + fn : "");
    }
    fclose(in);
    return file_save_end(out, file, 1) == 0 || !changed ? 0 : -1;
}
