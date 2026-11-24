#include "cred.h"

#include <string.h>

#include "secret.h"
#include "util.h"

int cred_pack(const char *plain, char *out, size_t len)
{
    if (len)
        out[0] = 0;
    if (!plain || !*plain)
        return 0;
    if (secret_seal(plain, out, len) != 0) {
        if (len)
            out[0] = 0;
        return -1;
    }
    return 0;
}

int cred_unpack(const char *stored, char *out, size_t len)
{
    if (!stored || !*stored) {
        if (len)
            out[0] = 0;
        return CRED_OK;
    }
    int sealed = secret_is_sealed(stored);
    if (secret_open(stored, out, len) != 0) {
        if (len)
            out[0] = 0;
        return CRED_BAD;
    }
    return sealed ? CRED_OK : CRED_OLD;
}

int cred_config_get(const config *c, const char *key, char *out, size_t len)
{
    return cred_unpack(config_get(c, key, ""), out, len);
}

void cred_config_put(FILE *f, const char *key, const char *plain)
{
    char sealed[1024];
    cred_pack(plain, sealed, sizeof sealed);
    fprintf(f, "%s=%s\n", key, sealed);
}

/* ---- Каталоги OPDS ---- */

static void clean_field(char *s)
{
    for (; *s; s++)
        if (*s == '\t' || *s == '\n' || *s == '\r')
            *s = ' ';
}

static void copy(char *dst, size_t n, const char *src)
{
    size_t l = strlen(src);
    if (l >= n)
        l = n - 1;
    memcpy(dst, src, l);
    dst[l] = 0;
}

int cred_cats_load(const char *path, cred_cat *out, int max, int *rewrite)
{
    int n = 0;
    *rewrite = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    char line[2048];
    while (n < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!*line || *line == '#')
            continue;
        cred_cat *c = &out[n];
        memset(c, 0, sizeof *c);
        char *fld[4] = { NULL, NULL, NULL, NULL };
        char *p = line;
        for (int i = 0; i < 4 && p; i++) {
            fld[i] = p;
            char *t = strchr(p, '\t');
            if (t)
                *t++ = 0;
            p = t;
        }
        if (!fld[1] || !*fld[1])
            continue;
        copy(c->name, sizeof c->name, fld[0]);
        copy(c->url, sizeof c->url, fld[1]);
        int a = cred_unpack(fld[2], c->login, sizeof c->login);
        int b = cred_unpack(fld[3], c->pass, sizeof c->pass);
        if (a == CRED_BAD || b == CRED_BAD) { /* пара — целиком, половинка бесполезна */
            c->login[0] = c->pass[0] = 0;
            c->lost = 1;
            *rewrite = 1; /* в файле — пусто, а не мусор */
        } else if (a == CRED_OLD || b == CRED_OLD) {
            *rewrite = 1;
        }
        n++;
    }
    fclose(f);
    return n;
}

int cred_cats_save(const char *path, const cred_cat *cats, int n)
{
    FILE *f = file_save_begin(path);
    if (!f)
        return -1;
    fputs("# Каталоги OPDS: название, адрес, логин и пароль (через табуляцию; логин и пароль зашифрованы)\n", f);
    for (int i = 0; i < n; i++) {
        cred_cat c = cats[i];
        clean_field(c.name);
        clean_field(c.url);
        char lg[512] = "", pw[512] = "";
        cred_pack(c.login, lg, sizeof lg);
        cred_pack(c.pass, pw, sizeof pw);
        if ((*c.login && !*lg) || (*c.pass && !*pw)) /* не зашифровалось — пара не пишется вовсе */
            lg[0] = pw[0] = 0;
        fprintf(f, "%s\t%s\t%s\t%s\n", c.name, c.url, lg, pw);
    }
    return file_save_end(f, path, 1);
}
