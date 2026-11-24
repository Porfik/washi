/* _DEFAULT_SOURCE: strcasecmp. */
#define _DEFAULT_SOURCE
#include "syncjob.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../base/log.h"
#include "../base/util.h"
#include "net.h"

#define MAX_DIRS 3000
#define MAX_FILES 20000
#define MAX_DEPTH 12
#define KEEP_FREE (24LL << 20) /* столько места оставить на книге */
#define NOTES_DIR ".washi"

struct sync_job {
    sync_job_cfg cfg;
    pthread_t th;
    int started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    volatile int cancel;
    sync_status st;
    int merged, merge_ok;
    notes_result mr;
    notes_files files;
    char path[8][1100];
    char state[1100], notes_base[1100];
};

/* ---- Книжные форматы ---- */

int sync_is_book(const char *name)
{
    /* Те же форматы, что показывает «Библиотека» (formats в src/ui/library.c). */
    static const char *const exts[] = { ".pdf", ".epub", ".fb2",  ".fb2.zip", ".djvu", ".djv", ".mobi", ".azw3",
                                        ".rtf", ".doc",  ".docx", ".cbz",     ".cbr",  ".chm" };
    size_t n = strlen(name);
    for (size_t i = 0; i < sizeof exts / sizeof *exts; i++) {
        size_t l = strlen(exts[i]);
        if (n > l && !strcasecmp(name + n - l, exts[i]))
            return 1;
    }
    return 0;
}

/* ---- Состояние скачанных книг ---- */

typedef struct {
    char *rel;
    long long ssize, smtime; /* на сервере при скачивании */
    long long lsize, lmtime; /* на книге после скачивания */
} rec;

typedef struct {
    rec *v;
    int n, cap;
} recs;

static int recs_add(recs *r, const char *rel, long long ss, long long sm, long long ls, long long lm)
{
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 256;
        rec *v = realloc(r->v, sizeof *v * (size_t)cap);
        if (!v)
            return -1;
        r->v = v;
        r->cap = cap;
    }
    char *k = strdup(rel);
    if (!k)
        return -1;
    r->v[r->n++] = (rec){ k, ss, sm, ls, lm };
    return 0;
}

static void recs_free(recs *r)
{
    for (int i = 0; i < r->n; i++)
        free(r->v[i].rel);
    free(r->v);
    memset(r, 0, sizeof *r);
}

static int rec_cmp(const void *a, const void *b)
{
    return strcmp(((const rec *)a)->rel, ((const rec *)b)->rel);
}

/* Строки «путь<TAB>размер на сервере<TAB>время<TAB>размер на книге<TAB>время». */
static void state_load(recs *r, const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return;
    char line[1500];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t[4];
        char *p = line;
        int n = 0;
        while (n < 4 && (p = strchr(p, '\t')) != NULL) {
            *p++ = 0;
            t[n++] = p;
        }
        if (n < 4 || !line[0])
            continue;
        recs_add(r, line, atoll(t[0]), atoll(t[1]), atoll(t[2]), atoll(t[3]));
    }
    fclose(f);
    qsort(r->v, (size_t)r->n, sizeof *r->v, rec_cmp);
}

static void state_save(const recs *r, const char *file)
{
    FILE *f = file_save_begin(file);
    if (!f)
        return;
    for (int i = 0; i < r->n; i++)
        fprintf(f, "%s\t%lld\t%lld\t%lld\t%lld\n", r->v[i].rel, r->v[i].ssize, r->v[i].smtime, r->v[i].lsize,
                r->v[i].lmtime);
    file_save_end(f, file, 0);
}

static const rec *recs_find(const recs *r, const char *rel)
{
    rec key = { (char *)rel, 0, 0, 0, 0 };
    return bsearch(&key, r->v, (size_t)r->n, sizeof *r->v, rec_cmp);
}

/* ---- Обход сервера ---- */

typedef struct {
    char *rel;
    long long size, mtime;
    const rec *old;
    int done; /* обработана (скачана или признана нетребующей) */
} book;

typedef struct {
    book *v;
    int n, cap;
} books;

static int books_add(books *b, const char *rel, long long size, long long mtime)
{
    if (b->n == b->cap) {
        int cap = b->cap ? b->cap * 2 : 256;
        book *v = realloc(b->v, sizeof *v * (size_t)cap);
        if (!v)
            return -1;
        b->v = v;
        b->cap = cap;
    }
    char *k = strdup(rel);
    if (!k)
        return -1;
    b->v[b->n++] = (book){ k, size, mtime, NULL, 0 };
    return 0;
}

static void books_free(books *b)
{
    for (int i = 0; i < b->n; i++)
        free(b->v[i].rel);
    free(b->v);
    memset(b, 0, sizeof *b);
}

/* Путь на книге: каждая часть — безопасное для FAT32 имя. */
static void local_path(const char *dir, const char *rel, char *out, size_t len)
{
    snprintf(out, len, "%s", dir);
    char part[1100];
    snprintf(part, sizeof part, "%s", rel);
    for (char *s = part; s;) {
        char *slash = strchr(s, '/');
        if (slash)
            *slash = 0;
        char safe[256];
        net_safe_name(s, safe, sizeof safe);
        size_t n = strlen(out);
        snprintf(out + n, len - n, "/%s", safe);
        s = slash ? slash + 1 : NULL;
    }
}

static void set_cur(sync_job *j, const char *name, long long total)
{
    pthread_mutex_lock(&j->mu);
    snprintf(j->st.cur, sizeof j->st.cur, "%s", name);
    j->st.cur_done = 0;
    j->st.cur_total = total;
    pthread_mutex_unlock(&j->mu);
}

static void on_progress(void *user, long long done, long long total)
{
    sync_job *j = user;
    pthread_mutex_lock(&j->mu);
    j->st.cur_done = done;
    if (total > 0)
        j->st.cur_total = total;
    pthread_mutex_unlock(&j->mu);
}

static void set_stage(sync_job *j, int stage)
{
    pthread_mutex_lock(&j->mu);
    j->st.stage = stage;
    pthread_mutex_unlock(&j->mu);
}

static void fail(sync_job *j, const char *text)
{
    pthread_mutex_lock(&j->mu);
    snprintf(j->st.error, sizeof j->st.error, "%s", text);
    pthread_mutex_unlock(&j->mu);
}

/* Обойти папки сервера. 0 — обошли (возможно, не все папки открылись, n_dirs_failed);
 * -1 — не вышло совсем (error). */
static int scan(sync_job *j, books *out)
{
    const remote_cfg *c = &j->cfg.remote;
    char **queue = calloc(MAX_DIRS, sizeof *queue);
    if (!queue)
        return -1;
    int qn = 0, qi = 0, rc = 0, fails_in_row = 0;
    queue[qn++] = strdup("");
    while (qi < qn && !j->cancel) {
        char *dir = queue[qi++];
        remote_entries e = { NULL, 0, 0 };
        char err[256];
        if (remote_list(c, dir, &e, &j->cancel, err, sizeof err, NULL) != 0) {
            remote_entries_free(&e);
            if (j->cancel)
                break;
            log_msg("синхронизация: папка «%s»: %s", dir, err);
            if (!*dir || ++fails_in_row >= 3) {
                fail(j, err);
                rc = -1;
                break;
            }
            pthread_mutex_lock(&j->mu);
            j->st.n_dirs_failed++;
            pthread_mutex_unlock(&j->mu);
            free(dir);
            continue;
        }
        fails_in_row = 0;
        int depth = 0;
        for (const char *s = dir; *s; s++)
            depth += *s == '/';
        for (int i = 0; i < e.count; i++) {
            const remote_entry *it = &e.items[i];
            if (!it->name[0] || it->name[0] == '.' || strchr(it->name, '/') || strchr(it->name, '\\'))
                continue; /* скрытое (в том числе наша .washi) и странные имена */
            char rel[1100];
            snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", it->name);
            if (it->is_dir) {
                if (qn < MAX_DIRS && depth < MAX_DEPTH)
                    queue[qn++] = strdup(rel);
            } else if (sync_is_book(it->name) && out->n < MAX_FILES) {
                if (books_add(out, rel, it->size, it->mtime) != 0)
                    rc = -1;
            }
        }
        remote_entries_free(&e);
        pthread_mutex_lock(&j->mu);
        j->st.dirs = qi;
        j->st.files = out->n;
        pthread_mutex_unlock(&j->mu);
        free(dir);
    }
    for (int i = qi; i < qn; i++)
        free(queue[i]);
    free(queue);
    return rc;
}

/* ---- Книги ---- */

/* Нужно ли качать b (и какое слово писать): 0 — нет, 1 — новая, 2 — обновление.
 * Решает по состоянию и файлу на книге; adopt — занести в состояние как есть. */
static int decide(sync_job *j, const book *b, const char *local, int *adopt, long long *lsize, long long *lmtime)
{
    struct stat st;
    *adopt = 0;
    if (stat(local, &st) != 0 || !S_ISREG(st.st_mode))
        return 1; /* нет на книге: качаем (в том числе удалённую с книги) */
    *lsize = st.st_size;
    *lmtime = st.st_mtime;
    if (!b->old) {
        if (b->size < 0 || st.st_size == b->size) {
            *adopt = 1; /* такой же файл уже есть */
            return 0;
        }
        pthread_mutex_lock(&j->mu);
        j->st.n_conflict++;
        pthread_mutex_unlock(&j->mu);
        return 0;
    }
    int local_changed = st.st_size != b->old->lsize || st.st_mtime != b->old->lmtime;
    int server_changed = (b->size >= 0 && b->size != b->old->ssize) ||
                         (b->mtime && b->old->smtime && b->mtime != b->old->smtime);
    if (!server_changed)
        return 0;
    if (local_changed) {
        pthread_mutex_lock(&j->mu);
        j->st.n_conflict++;
        pthread_mutex_unlock(&j->mu);
        return 0;
    }
    return 2;
}

/* Свободно ли на книге size байт (папка dir может ещё не существовать — смотрим вверх). */
static int has_space(const char *dir, long long size)
{
    char p[1100];
    snprintf(p, sizeof p, "%s", dir);
    struct statvfs sv;
    while (statvfs(p, &sv) != 0) {
        char *s = strrchr(p, '/');
        if (!s || s == p)
            return 1; /* не узнать — пробуем */
        *s = 0;
    }
    return (long long)sv.f_bavail * (long long)sv.f_frsize >= size + KEEP_FREE;
}

static void do_books(sync_job *j, books *bs, const recs *old, recs *out)
{
    const remote_cfg *c = &j->cfg.remote;
    typedef struct {
        int idx, kind;
        char local[1100];
    } job_item;
    job_item *todo = calloc((size_t)(bs->n ? bs->n : 1), sizeof *todo);
    int nt = 0;
    long long total = 0;
    if (!todo)
        return;
    for (int i = 0; i < bs->n; i++) {
        book *b = &bs->v[i];
        b->old = recs_find(old, b->rel);
        char local[1100];
        local_path(j->cfg.local_dir, b->rel, local, sizeof local);
        int adopt;
        long long ls = 0, lm = 0;
        int kind = decide(j, b, local, &adopt, &ls, &lm);
        if (kind) {
            todo[nt].idx = i;
            todo[nt].kind = kind;
            snprintf(todo[nt].local, sizeof todo[nt].local, "%s", local);
            nt++;
            total += b->size > 0 ? b->size : 0;
        } else {
            if (adopt)
                recs_add(out, b->rel, b->size, b->mtime, ls, lm);
            else if (b->old)
                recs_add(out, b->rel, b->old->ssize, b->old->smtime, b->old->lsize, b->old->lmtime);
            b->done = 1;
        }
    }
    pthread_mutex_lock(&j->mu);
    j->st.todo = nt;
    j->st.bytes_total = total;
    j->st.stage = SJ_BOOKS;
    pthread_mutex_unlock(&j->mu);

    int fails_in_row = 0;
    for (int k = 0; k < nt && !j->cancel; k++) {
        book *b = &bs->v[todo[k].idx];
        const char *slash = strrchr(b->rel, '/');
        set_cur(j, slash ? slash + 1 : b->rel, b->size);
        char dir[1100];
        snprintf(dir, sizeof dir, "%s", todo[k].local);
        *strrchr(dir, '/') = 0;
        net_mkdirs(dir);
        char err[256] = "";
        int ok = 0;
        if (b->size > 0 && !has_space(dir, b->size)) {
            pthread_mutex_lock(&j->mu);
            j->st.n_nospace++;
            pthread_mutex_unlock(&j->mu);
        } else if (remote_get(c, b->rel, todo[k].local, &j->cancel, on_progress, j, err, sizeof err, NULL) == 0) {
            ok = 1;
        } else if (!j->cancel) {
            log_msg("синхронизация: «%s»: %s", b->rel, err);
            pthread_mutex_lock(&j->mu);
            j->st.n_failed++;
            if (!j->st.first_fail[0])
                snprintf(j->st.first_fail, sizeof j->st.first_fail, "%s", err);
            pthread_mutex_unlock(&j->mu);
        }
        if (ok) {
            fails_in_row = 0;
            struct stat st;
            long long ls = 0, lm = 0;
            if (stat(todo[k].local, &st) == 0) {
                ls = st.st_size;
                lm = st.st_mtime;
            }
            recs_add(out, b->rel, b->size >= 0 ? b->size : ls, b->mtime, ls, lm);
            b->done = 1;
            pthread_mutex_lock(&j->mu);
            if (todo[k].kind == 1)
                j->st.n_new++;
            else
                j->st.n_updated++;
            j->st.bytes_done += b->size > 0 ? b->size : ls;
            j->st.done = k + 1;
            pthread_mutex_unlock(&j->mu);
        } else {
            pthread_mutex_lock(&j->mu);
            j->st.done = k + 1;
            if (!j->cancel && !(err[0] == 0) && ++fails_in_row >= 3) {
                snprintf(j->st.error, sizeof j->st.error, "%s", err);
                j->cancel = 1; /* связи нет — дальше нет смысла */
                j->st.cancelled = 0;
            }
            pthread_mutex_unlock(&j->mu);
        }
    }
    /* не дошли до книги — оставляем прежнюю запись состояния */
    for (int i = 0; i < bs->n; i++)
        if (!bs->v[i].done && bs->v[i].old)
            recs_add(out, bs->v[i].rel, bs->v[i].old->ssize, bs->v[i].old->smtime, bs->v[i].old->lsize,
                     bs->v[i].old->lmtime);
    free(todo);
}

/* ---- Места чтения и закладки ---- */

/* Скачать файл заметок; 404/550 — на сервере его ещё нет (файл удаляется). 0 — готово. */
static int fetch_note(sync_job *j, const char *name, const char *local, char *err, size_t err_len)
{
    char rel[128];
    snprintf(rel, sizeof rel, NOTES_DIR "/%s", name);
    remove(local);
    long status = 0;
    if (remote_get(&j->cfg.remote, rel, local, &j->cancel, NULL, NULL, err, err_len, &status) == 0)
        return 0;
    if (status == 404 || status == 550)
        return 0;
    return -1;
}

static void notes_stage(sync_job *j)
{
    char err[256] = "";
    set_stage(j, SJ_NOTES);
    set_cur(j, "", 0);
    if (fetch_note(j, "positions.txt", j->files.srv_pos, err, sizeof err) != 0 ||
        fetch_note(j, "bookmarks.txt", j->files.srv_bm, err, sizeof err) != 0) {
        if (!j->cancel) {
            pthread_mutex_lock(&j->mu);
            snprintf(j->st.notes_error, sizeof j->st.notes_error, "%s", err);
            pthread_mutex_unlock(&j->mu);
        }
        return;
    }
    pthread_mutex_lock(&j->mu);
    j->st.stage = SJ_MERGE;
    while (!j->merged && !j->cancel)
        pthread_cond_wait(&j->cv, &j->mu);
    int ok = j->merged && j->merge_ok;
    notes_result r = j->mr;
    int cancelled = j->cancel;
    if (!cancelled && !ok)
        snprintf(j->st.notes_error, sizeof j->st.notes_error, "Не удалось слить места чтения.");
    pthread_mutex_unlock(&j->mu);
    if (cancelled || !ok)
        return;

    set_stage(j, SJ_UPLOAD);
    int sent = 0;
    if (r.send_pos) {
        if (remote_put(&j->cfg.remote, NOTES_DIR "/positions.txt", j->files.up_pos, &j->cancel, err, sizeof err) != 0)
            goto failed;
        sent++;
    }
    if (r.send_bm) {
        if (remote_put(&j->cfg.remote, NOTES_DIR "/bookmarks.txt", j->files.up_bm, &j->cancel, err, sizeof err) != 0)
            goto failed;
        sent++;
    }
    rename(j->files.new_base, j->files.base); /* сошлось — это теперь «база» */
    pthread_mutex_lock(&j->mu);
    j->st.sent_notes = sent;
    pthread_mutex_unlock(&j->mu);
    return;
failed:
    if (!j->cancel) {
        pthread_mutex_lock(&j->mu);
        snprintf(j->st.notes_error, sizeof j->st.notes_error, "Не удалось отправить на сервер: %s", err);
        pthread_mutex_unlock(&j->mu);
    }
}

/* ---- Поток ---- */

static int name_cmp(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void *run(void *arg)
{
    sync_job *j = arg;
    books bs = { NULL, 0, 0 };
    recs old = { NULL, 0, 0 }, out = { NULL, 0, 0 };
    state_load(&old, j->state);

    if (scan(j, &bs) == 0 && !j->cancel) {
        pthread_mutex_lock(&j->mu);
        j->st.n_total_books = bs.n;
        int complete = j->st.n_dirs_failed == 0;
        pthread_mutex_unlock(&j->mu);
        /* пропавшие с сервера: запись состояния убирается, книга на устройстве остаётся */
        const char **names = malloc(sizeof *names * (size_t)(bs.n ? bs.n : 1));
        int gone = 0;
        for (int i = 0; names && i < bs.n; i++)
            names[i] = bs.v[i].rel;
        if (names)
            qsort(names, (size_t)bs.n, sizeof *names, name_cmp);
        for (int i = 0; names && i < old.n; i++)
            if (!bsearch(&old.v[i].rel, names, (size_t)bs.n, sizeof *names, name_cmp))
                gone++;
        if (complete) {
            pthread_mutex_lock(&j->mu);
            j->st.n_server_deleted = gone;
            pthread_mutex_unlock(&j->mu);
        }
        do_books(j, &bs, &old, &out);
        if (!complete) {
            /* часть папок не открылась — записи о книгах, которых не видели, не трогаем */
            for (int i = 0; names && i < old.n; i++)
                if (!bsearch(&old.v[i].rel, names, (size_t)bs.n, sizeof *names, name_cmp))
                    recs_add(&out, old.v[i].rel, old.v[i].ssize, old.v[i].smtime, old.v[i].lsize, old.v[i].lmtime);
        }
        free(names);
        state_save(&out, j->state);
        pthread_mutex_lock(&j->mu);
        int fatal = j->st.error[0];
        pthread_mutex_unlock(&j->mu);
        if (!fatal && !j->cancel)
            notes_stage(j);
    }
    pthread_mutex_lock(&j->mu);
    j->st.cancelled = j->cancel && !j->st.error[0];
    j->st.stage = SJ_DONE;
    pthread_mutex_unlock(&j->mu);
    books_free(&bs);
    recs_free(&old);
    recs_free(&out);
    return NULL;
}

static void make_path(sync_job *j, int i, const char *name)
{
    snprintf(j->path[i], sizeof j->path[i], "%s/%s", j->cfg.data_dir, name);
}

sync_job *sync_job_start(const sync_job_cfg *c)
{
    sync_job *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;
    j->cfg = *c;
    size_t n = strlen(j->cfg.local_dir);
    while (n > 1 && j->cfg.local_dir[n - 1] == '/')
        j->cfg.local_dir[--n] = 0;
    pthread_mutex_init(&j->mu, NULL);
    pthread_cond_init(&j->cv, NULL);
    make_path(j, 0, "sync-srv-positions.txt");
    make_path(j, 1, "sync-srv-bookmarks.txt");
    make_path(j, 2, "sync-up-positions.txt");
    make_path(j, 3, "sync-up-bookmarks.txt");
    make_path(j, 4, "sync-notes.txt");
    make_path(j, 5, "sync-notes.new");
    make_path(j, 6, "bookmarks.txt");
    make_path(j, 7, "sync-books.txt");
    j->files = (notes_files){ j->cfg.local_dir, j->path[0], j->path[1], j->path[6], j->path[4],
                              j->path[2],       j->path[3], j->path[5] };
    snprintf(j->state, sizeof j->state, "%s", j->path[7]);
    j->st.stage = SJ_LIST;
    if (pthread_create(&j->th, NULL, run, j) != 0) {
        free(j);
        return NULL;
    }
    j->started = 1;
    return j;
}

void sync_job_status(sync_job *j, sync_status *out)
{
    pthread_mutex_lock(&j->mu);
    *out = j->st;
    pthread_mutex_unlock(&j->mu);
}

void sync_job_cancel(sync_job *j)
{
    pthread_mutex_lock(&j->mu);
    j->cancel = 1;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
}

const notes_files *sync_job_files(const sync_job *j)
{
    return &j->files;
}

void sync_job_merged(sync_job *j, int ok, const notes_result *r)
{
    pthread_mutex_lock(&j->mu);
    j->merged = 1;
    j->merge_ok = ok == 0;
    if (r)
        j->mr = *r;
    pthread_cond_broadcast(&j->cv);
    pthread_mutex_unlock(&j->mu);
}

void sync_job_free(sync_job *j)
{
    if (!j)
        return;
    sync_job_cancel(j);
    if (j->started)
        pthread_join(j->th, NULL);
    pthread_mutex_destroy(&j->mu);
    pthread_cond_destroy(&j->cv);
    free(j);
}
