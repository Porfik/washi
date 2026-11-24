#include "positions.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int positions_load(positions *p, const char *file)
{
    memset(p, 0, sizeof *p);
    snprintf(p->file, sizeof p->file, "%s", file);
    p->items = calloc(POSITIONS_MAX, sizeof *p->items);
    if (!p->items)
        return -1;
    FILE *f = fopen(file, "r");
    if (!f)
        return 0;
    char line[1400];
    while (p->count < POSITIONS_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t[5] = { NULL, NULL, NULL, NULL, NULL };
        char *s = line;
        int n = 0;
        while (n < 5 && (s = strchr(s, '\t')) != NULL)
            t[n++] = s++;
        if (n < 2)
            continue;
        position *it = &p->items[p->count];
        memset(it, 0, sizeof *it);
        it->page = atoi(line) - 1;
        it->count = atoi(t[0] + 1);
        if (it->page < 0)
            it->page = 0;
        const char *path = t[1] + 1;
        if (n >= 4) {
            /* новый вид: масштаб и поля (и вид) перед путём */
            it->view = atoi(t[1] + 1);
            it->has_crop = sscanf(t[2] + 1, "%f,%f,%f,%f,%f,%f,%f,%f", &it->crop[0], &it->crop[1], &it->crop[2],
                                  &it->crop[3], &it->crop[4], &it->crop[5], &it->crop[6], &it->crop[7]) == 8;
            path = t[3] + 1;
        }
        if (n == 5) {
            for (const char *f = t[3] + 1; f < t[4]; f++) {
                if (f[1] != '=')
                    continue;
                int v = atoi(f + 2);
                switch (*f) {
                case 'o': it->orient = v; break;
                case 's': it->spread = v; break;
                case 'c': it->contrast = v; break;
                case 'f': it->finished = v; break;
                }
            }
            path = t[4] + 1;
        }
        if (!*path)
            continue;
        snprintf(it->path, sizeof it->path, "%s", path);
        p->count++;
    }
    fclose(f);
    return 0;
}

void positions_free(positions *p)
{
    free(p->items);
    p->items = NULL;
    p->count = 0;
}

const position *positions_find(const positions *p, const char *path)
{
    for (int i = 0; i < p->count; i++)
        if (!strcmp(p->items[i].path, path))
            return &p->items[i];
    return NULL;
}

void positions_set(positions *p, const char *path, int page, int count)
{
    if (!p->items)
        return;
    int i = 0;
    while (i < p->count && strcmp(p->items[i].path, path))
        i++;
    position keep;
    memset(&keep, 0, sizeof keep);
    if (i == p->count) {
        if (p->count < POSITIONS_MAX)
            p->count++;
        i = p->count - 1; /* список полон — вытесняется самая давняя книга */
    } else {
        keep = p->items[i];
    }
    memmove(&p->items[1], &p->items[0], sizeof *p->items * (size_t)i);
    p->items[0] = keep;
    snprintf(p->items[0].path, sizeof p->items[0].path, "%s", path);
    p->items[0].page = page;
    p->items[0].count = count;
}

void positions_set_view(positions *p, const char *path, int view, const float *crop)
{
    for (int i = 0; i < p->count; i++) {
        if (strcmp(p->items[i].path, path))
            continue;
        p->items[i].view = view;
        if (crop) {
            memcpy(p->items[i].crop, crop, sizeof p->items[i].crop);
            p->items[i].has_crop = 1;
        }
        return;
    }
}

static position *find_rw(positions *p, const char *path)
{
    for (int i = 0; i < p->count; i++)
        if (!strcmp(p->items[i].path, path))
            return &p->items[i];
    return NULL;
}

void positions_set_display(positions *p, const char *path, int orient, int spread, int contrast)
{
    position *it = find_rw(p, path);
    if (it) {
        it->orient = orient;
        it->spread = spread;
        it->contrast = contrast;
    }
}

void positions_set_finished(positions *p, const char *path, int finished)
{
    position *it = find_rw(p, path);
    if (it)
        it->finished = finished;
}

void positions_remove(positions *p, const char *path)
{
    for (int i = 0; i < p->count; i++)
        if (!strcmp(p->items[i].path, path)) {
            memmove(&p->items[i], &p->items[i + 1], sizeof *p->items * (size_t)(p->count - i - 1));
            p->count--;
            return;
        }
}

/* path лежит под папкой dir (dir/…). */
static int under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    return !strncmp(path, dir, n) && path[n] == '/';
}

void positions_rename(positions *p, const char *from, const char *to)
{
    if (!strcmp(from, to))
        return;
    position *it = find_rw(p, from);
    if (!it)
        return;
    positions_remove(p, to); /* остаток от прежней книги с таким именем */
    it = find_rw(p, from);   /* список мог сдвинуться */
    snprintf(it->path, sizeof it->path, "%s", to);
}

void positions_rename_prefix(positions *p, const char *from, const char *to)
{
    size_t n = strlen(from);
    for (int i = 0; i < p->count; i++) {
        if (!under(p->items[i].path, from))
            continue;
        char np[1024];
        if (snprintf(np, sizeof np, "%s%s", to, p->items[i].path + n) >= (int)sizeof np) {
            memmove(&p->items[i], &p->items[i + 1], sizeof *p->items * (size_t)(p->count - i - 1));
            p->count--;
            i--;
            continue;
        }
        memcpy(p->items[i].path, np, strlen(np) + 1);
    }
}

int positions_save(const positions *p)
{
    FILE *f = file_save_begin(p->file);
    if (!f)
        return -1;
    for (int i = 0; i < p->count; i++) {
        const position *it = &p->items[i];
        fprintf(f, "%d\t%d\t%d\t", it->page + 1, it->count, it->view);
        if (it->has_crop)
            fprintf(f, "%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f", it->crop[0], it->crop[1], it->crop[2], it->crop[3],
                    it->crop[4], it->crop[5], it->crop[6], it->crop[7]);
        else
            fputc('-', f);
        fprintf(f, "\to=%d,s=%d,c=%d,f=%d\t%s\n", it->orient, it->spread, it->contrast, it->finished, it->path);
    }
    return file_save_end(f, p->file, 1);
}
