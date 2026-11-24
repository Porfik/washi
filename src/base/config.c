#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"

int config_load(config *c, const char *path)
{
    c->count = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    char line[1024];
    while (fgets(line, sizeof line, f) && c->count < CONFIG_MAX_ENTRIES) {
        char *s = str_trim(line);
        char *eq = strchr(s, '=');
        if (*s == '#' || !eq)
            continue;
        *eq = 0;
        snprintf(c->entries[c->count].key, sizeof c->entries[0].key, "%s", str_trim(s));
        snprintf(c->entries[c->count].value, sizeof c->entries[0].value, "%s", str_trim(eq + 1));
        c->count++;
    }
    fclose(f);
    return 0;
}

const char *config_get(const config *c, const char *key, const char *def)
{
    for (int i = c->count - 1; i >= 0; i--)
        if (!strcmp(c->entries[i].key, key))
            return c->entries[i].value;
    return def;
}

int config_get_int(const config *c, const char *key, int def)
{
    const char *v = config_get(c, key, NULL);
    return v && *v ? atoi(v) : def;
}

int config_save_value(const char *path, const char *key, const char *value)
{
    FILE *in = fopen(path, "r");
    FILE *out = file_save_begin(path);
    if (!out) {
        if (in)
            fclose(in);
        return -1;
    }
    int done = 0;
    char line[1024];
    while (in && fgets(line, sizeof line, in)) {
        char copy[1024];
        snprintf(copy, sizeof copy, "%s", line);
        char *s = str_trim(copy);
        char *eq = strchr(s, '=');
        if (!done && *s != '#' && eq) {
            *eq = 0;
            if (!strcmp(str_trim(s), key)) {
                fprintf(out, "%s=%s\n", key, value);
                done = 1;
                continue;
            }
        }
        fputs(line, out);
        if (!strchr(line, '\n'))
            fputc('\n', out);
    }
    if (in)
        fclose(in);
    if (!done)
        fprintf(out, "%s=%s\n", key, value);
    return file_save_end(out, path, 1);
}
