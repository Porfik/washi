#include "util.h"

#include <string.h>
#include <unistd.h>

#define TMP_MAX_PATH 1200

static int tmp_name(char *tmp, const char *path)
{
    int n = snprintf(tmp, TMP_MAX_PATH, "%s.tmp", path);
    return n > 0 && n < TMP_MAX_PATH ? 0 : -1;
}

FILE *file_save_begin(const char *path)
{
    char tmp[TMP_MAX_PATH];
    return tmp_name(tmp, path) ? NULL : fopen(tmp, "wb");
}

int file_save_end(FILE *f, const char *path, int sync)
{
    char tmp[TMP_MAX_PATH];
    if (!f || tmp_name(tmp, path))
        return -1;
    int ok = !ferror(f) && fflush(f) == 0 && (!sync || fsync(fileno(f)) == 0);
    ok &= fclose(f) == 0;
    if (ok && rename(tmp, path) == 0)
        return 0;
    unlink(tmp);
    return -1;
}

char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;
    return s;
}
