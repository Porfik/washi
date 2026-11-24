/* _DEFAULT_SOURCE: DT_*, lstat и S_ISLNK. */
#define _DEFAULT_SOURCE
#include "bookfiles.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../net/net.h"
#include "bookmarks.h"
#include "log.h"

const char *bookfiles_message(bf_result r)
{
    switch (r) {
    case BF_OK:
    case BF_SAME: return "";
    case BF_EXISTS: return "Такое имя уже занято. Выберите другое.";
    case BF_INVALID: return "Имя не может быть пустым.";
    case BF_OUTSIDE: return "Это не в библиотеке — трогать нельзя.";
    default: return "Не получилось: устройство отказало. Попробуйте ещё раз.";
    }
}

/* Расширение имени: «.fb2.zip» целиком, иначе от последней точки. Пусто — нет. */
static const char *ext_of(const char *name)
{
    size_t n = strlen(name);
    if (n > 8 && !strcasecmp(name + n - 8, ".fb2.zip"))
        return name + n - 8;
    const char *dot = strrchr(name, '.');
    return dot && dot != name && strlen(dot) <= 8 ? dot : name + n;
}

static const char *base_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

void bookfiles_stem(const char *path, char *out, size_t len)
{
    const char *b = base_of(path);
    snprintf(out, len, "%.*s", (int)(ext_of(b) - b), b);
}

/* Папки корня, которые не наши (прошивка, хаб): их нет в «Библиотеке», трогать нельзя. */
static int firmware_dir(const char *name)
{
    static const char *const names[] = { "system", "applications", "washi", "fonts", "screensavers",
                                         "System Volume Information" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++)
        if (!strcasecmp(name, names[i]))
            return 1;
    return 0;
}

/* path строго внутри root: папка, где он лежит, — внутри корня или сам корень (после realpath, то есть
 * ссылки наружу не годятся), имя не «.»/«..», первая часть пути под корнем — не папка прошивки.
 * Сам последний элемент не разворачивается. 0 — можно. В parent — настоящая папка, где он лежит. */
static int check_inside(const char *root, const char *path, char *parent, size_t plen)
{
    char rr[PATH_MAX];
    if (!realpath(root, rr))
        return -1;
    size_t rl = strlen(rr);
    while (rl > 1 && rr[rl - 1] == '/')
        rr[--rl] = 0;
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1] || !strcmp(slash + 1, ".") || !strcmp(slash + 1, ".."))
        return -1;
    char dir[PATH_MAX];
    size_t dn = (size_t)(slash - path);
    if (dn == 0 || dn >= sizeof dir)
        return -1;
    memcpy(dir, path, dn);
    dir[dn] = 0;
    char rd[PATH_MAX];
    if (!realpath(dir, rd))
        return -1;
    int at_root = !strcmp(rd, rr);
    if (!at_root && !(strncmp(rd, rr, rl) == 0 && (rd[rl] == '/' || rl == 1)))
        return -1;
    /* Первая часть под корнем: либо папка, где лежит путь, либо сам элемент. */
    const char *rel = at_root ? slash + 1 : rd + rl + (rl == 1 ? 0 : 1);
    char first[256];
    size_t fl = strcspn(rel, "/");
    if (fl >= sizeof first)
        return -1;
    memcpy(first, rel, fl);
    first[fl] = 0;
    if (firmware_dir(first))
        return -1;
    snprintf(parent, plen, "%s", rd);
    return 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return lstat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static void save_pos(positions *pos)
{
    if (positions_save(pos) != 0)
        log_msg("файлы: не записать %s", pos->file);
}

/* Новое имя в папке parent: stem + ext, прошедшее net_safe_name. Пусто — BF_INVALID. */
static bf_result new_name(const char *typed, const char *ext, char *out, size_t len)
{
    char t[600];
    snprintf(t, sizeof t, "%s", typed);
    size_t n = strlen(t);
    while (n && (t[n - 1] == ' ' || t[n - 1] == '\t' || t[n - 1] == '\n'))
        t[--n] = 0;
    char *s = t;
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    size_t el = strlen(ext);
    if (el && n > el && !strcasecmp(s + n - el, ext))
        s[n - el] = 0; /* дописал «.pdf» сам */
    if (!*s || strspn(s, ". ") == strlen(s))
        return BF_INVALID;
    char safe[300];
    net_safe_name(s, safe, sizeof safe);
    if (snprintf(out, len, "%s%s", safe, ext) >= (int)len)
        return BF_INVALID;
    return BF_OK;
}

/* Занят ли путь to кем-то другим, кроме from (на FAT «А.pdf» и «а.pdf» — один файл). */
static int taken(const char *from, const char *to)
{
    struct stat a, b;
    if (lstat(to, &b) != 0)
        return 0;
    return !(lstat(from, &a) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino);
}

bf_result bookfiles_rename_book(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                                const char *path, const char *stem, char *newpath, size_t len)
{
    char parent[PATH_MAX];
    struct stat st;
    if (check_inside(root, path, parent, sizeof parent) != 0 || lstat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return BF_OUTSIDE;
    snprintf(newpath, len, "%s", path);
    const char *b = base_of(path);
    char name[400], to[1100];
    bf_result r = new_name(stem, ext_of(b), name, sizeof name);
    if (r != BF_OK)
        return r;
    int dn = (int)(b - path);
    if (snprintf(to, sizeof to, "%.*s%s", dn, path, name) >= (int)sizeof to)
        return BF_INVALID;
    if (!strcmp(to, path))
        return BF_SAME;
    if (taken(path, to))
        return BF_EXISTS;
    if (rename(path, to) != 0) {
        log_msg("файлы: переименовать %s в %s: %s", path, to, strerror(errno));
        return BF_FAILED;
    }
    log_msg("файлы: книга %s → %s", path, to);
    positions_rename(pos, path, to);
    save_pos(pos);
    bookmarks_move(bookmarks_file, path, to, 0);
    if (cv)
        covers_move(cv, path, to);
    snprintf(newpath, len, "%s", to);
    return BF_OK;
}

bf_result bookfiles_delete_book(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                                const char *path)
{
    char parent[PATH_MAX];
    struct stat st;
    if (check_inside(root, path, parent, sizeof parent) != 0 || lstat(path, &st) != 0 || S_ISDIR(st.st_mode))
        return BF_OUTSIDE;
    if (cv)
        covers_forget(cv, path);
    if (unlink(path) != 0) {
        log_msg("файлы: удалить %s: %s", path, strerror(errno));
        return BF_FAILED;
    }
    log_msg("файлы: удалена книга %s", path);
    positions_remove(pos, path);
    save_pos(pos);
    bookmarks_move(bookmarks_file, path, NULL, 0);
    return BF_OK;
}

/* Обойти папку: для каждого обычного файла — f(cv, путь по старому имени, путь по новому). */
typedef void (*walk_fn)(covers *cv, const char *a, const char *b);

static void walk(const char *dir, const char *old_dir, covers *cv, walk_fn f, int depth)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return;
    struct dirent *e;
    char p[1100], q[1100];
    while ((e = readdir(dp)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p ||
            snprintf(q, sizeof q, "%s/%s", old_dir, e->d_name) >= (int)sizeof q)
            continue;
        struct stat st;
        if (lstat(p, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 32)
                walk(p, q, cv, f, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            f(cv, q, p);
        }
    }
    closedir(dp);
}

static void walk_move(covers *cv, const char *old_path, const char *new_path)
{
    covers_move(cv, old_path, new_path);
}

static void walk_forget(covers *cv, const char *path, const char *same)
{
    (void)same;
    covers_forget(cv, path);
}

bf_result bookfiles_rename_dir(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                               const char *dir, const char *name, char *newdir, size_t len)
{
    char parent[PATH_MAX];
    if (check_inside(root, dir, parent, sizeof parent) != 0 || !is_dir(dir))
        return BF_OUTSIDE;
    snprintf(newdir, len, "%s", dir);
    char safe[400], to[1100];
    bf_result r = new_name(name, "", safe, sizeof safe);
    if (r != BF_OK)
        return r;
    const char *b = base_of(dir);
    int dn = (int)(b - dir);
    if (snprintf(to, sizeof to, "%.*s%s", dn, dir, safe) >= (int)sizeof to)
        return BF_INVALID;
    /* Новое имя в корне не должно стать папкой прошивки. */
    char tp[PATH_MAX];
    if (check_inside(root, to, tp, sizeof tp) != 0)
        return BF_OUTSIDE;
    if (!strcmp(to, dir))
        return BF_SAME;
    if (taken(dir, to))
        return BF_EXISTS;
    if (rename(dir, to) != 0) {
        log_msg("файлы: переименовать %s в %s: %s", dir, to, strerror(errno));
        return BF_FAILED;
    }
    log_msg("файлы: папка %s → %s", dir, to);
    positions_rename_prefix(pos, dir, to);
    save_pos(pos);
    bookmarks_move(bookmarks_file, dir, to, 1);
    if (cv)
        walk(to, dir, cv, walk_move, 0);
    snprintf(newdir, len, "%s", to);
    return BF_OK;
}

/* Удалить папку со всем внутри; ссылки удаляются сами, наружу по ним не идём. 0 — всё удалено. */
static int rmtree(const char *dir, int depth)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return -1;
    int bad = 0;
    struct dirent *e;
    char p[1100];
    while ((e = readdir(dp)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (snprintf(p, sizeof p, "%s/%s", dir, e->d_name) >= (int)sizeof p) {
            bad = 1;
            continue;
        }
        struct stat st;
        if (lstat(p, &st) != 0) {
            bad = 1;
        } else if (S_ISDIR(st.st_mode)) {
            if (depth >= 32 || rmtree(p, depth + 1) != 0)
                bad = 1;
        } else if (unlink(p) != 0) {
            bad = 1;
        }
    }
    closedir(dp);
    return bad || rmdir(dir) != 0 ? -1 : 0;
}

bf_result bookfiles_delete_dir(const char *root, positions *pos, const char *bookmarks_file, covers *cv,
                               const char *dir)
{
    char parent[PATH_MAX];
    if (check_inside(root, dir, parent, sizeof parent) != 0 || !is_dir(dir))
        return BF_OUTSIDE;
    if (cv)
        walk(dir, dir, cv, walk_forget, 0);
    int rc = rmtree(dir, 0);
    log_msg("файлы: удалена папка %s%s", dir, rc ? " (не всё)" : "");
    /* Даже если часть не удалилась, забываем то, чего уже нет. */
    int k = 0;
    for (int i = 0; i < pos->count; i++) {
        const char *pp = pos->items[i].path;
        size_t n = strlen(dir);
        if (!(!strncmp(pp, dir, n) && pp[n] == '/' && access(pp, F_OK) != 0))
            pos->items[k++] = pos->items[i];
    }
    pos->count = k;
    save_pos(pos);
    if (rc == 0)
        bookmarks_move(bookmarks_file, dir, NULL, 1);
    return rc == 0 ? BF_OK : BF_FAILED;
}
