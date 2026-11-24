/* Синхронизация: разбор PROPFIND (WebDAV), списков FTP (MLSD и LIST), адрес сервера,
 * слияние мест чтения и закладок. Сети не нужно. */
#include "../../src/net/remote.h"
#include "../../src/net/syncjob.h"
#include "../../src/net/syncnotes.h"
#include "../../src/net/webdav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char *pf_data_dir(void) /* net.c ищет там cacert.pem */
{
    return "/nonexistent";
}

static int fails;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("НЕ ТАК (строка %d): ", __LINE__);     \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            fails++;                                      \
        }                                                 \
    } while (0)

static const remote_entry *find(const remote_entries *e, const char *name)
{
    for (int i = 0; i < e->count; i++)
        if (!strcmp(e->items[i].name, name))
            return &e->items[i];
    return NULL;
}

/* ---- PROPFIND ---- */

static const char nextcloud[] =
    "<?xml version=\"1.0\"?>\n"
    "<d:multistatus xmlns:d=\"DAV:\" xmlns:s=\"http://sabredav.org/ns\" xmlns:oc=\"http://owncloud.org/ns\">\n"
    "<d:response><d:href>/remote.php/dav/files/ivan/%D0%9A%D0%BD%D0%B8%D0%B3%D0%B8/</d:href>\n"
    "<d:propstat><d:prop><d:getlastmodified>Tue, 24 Nov 2026 10:30:00 GMT</d:getlastmodified>"
    "<d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
    "</d:response>\n"
    "<d:response><d:href>/remote.php/dav/files/ivan/%D0%9A%D0%BD%D0%B8%D0%B3%D0%B8/%D0%92%D0%BE%D0%B9%D0%BD%D0%B0%20%D0%B8%20%D0%BC%D0%B8%D1%80.fb2</d:href>\n"
    "<d:propstat><d:prop><d:getlastmodified>Mon, 02 Nov 2026 08:00:01 GMT</d:getlastmodified>"
    "<d:getcontentlength>1234567</d:getcontentlength><d:resourcetype/></d:prop>"
    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>\n"
    "<d:response><d:href>/remote.php/dav/files/ivan/%D0%9A%D0%BD%D0%B8%D0%B3%D0%B8/%D0%A4%D0%B0%D0%BD%D1%82%D0%B0%D1%81%D1%82%D0%B8%D0%BA%D0%B0/</d:href>\n"
    "<d:propstat><d:prop><d:getlastmodified>Mon, 02 Nov 2026 08:00:01 GMT</d:getlastmodified>"
    "<d:resourcetype><d:collection/></d:resourcetype></d:prop><d:status>HTTP/1.1 200 OK</d:status></d:propstat>"
    "</d:response>\n"
    "<d:response><d:href>/remote.php/dav/files/ivan/%D0%9A%D0%BD%D0%B8%D0%B3%D0%B8/Tom%20%26%20Jerry.pdf</d:href>\n"
    "<d:propstat><d:prop><d:getcontentlength>42</d:getcontentlength></d:prop>"
    "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>\n"
    "</d:multistatus>\n";

/* Apache: D:, абсолютные href с хостом, &amp; в имени, lp1:, пустые элементы в 404-блоке. */
static const char apache[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<D:multistatus xmlns:D=\"DAV:\"><D:response xmlns:lp1=\"DAV:\">\n"
    "<D:href>https://srv.example/dav/books/</D:href>\n"
    "<D:propstat><D:prop><lp1:resourcetype><D:collection/></lp1:resourcetype></D:prop>"
    "<D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>\n"
    "<D:response xmlns:lp1=\"DAV:\"><D:href>https://srv.example/dav/books/A&amp;B.epub</D:href>\n"
    "<D:propstat><D:prop><lp1:getcontentlength>10</lp1:getcontentlength>"
    "<lp1:getlastmodified>Sat, 01 Jan 2000 00:00:00 GMT</lp1:getlastmodified></D:prop>"
    "<D:status>HTTP/1.1 200 OK</D:status></D:propstat>"
    "<D:propstat><D:prop><lp1:resourcetype/></D:prop><D:status>HTTP/1.1 404 Not Found</D:status></D:propstat>"
    "</D:response></D:multistatus>";

/* Без префикса, пространство имён по умолчанию (IIS и др.), заглавные теги, CDATA. */
static const char bare[] =
    "<?xml version=\"1.0\"?><multistatus xmlns=\"DAV:\">"
    "<response><href>/</href><propstat><prop><resourcetype><collection/></resourcetype></prop></propstat></response>"
    "<response><href><![CDATA[/a b.txt]]></href><propstat><prop><getcontentlength>7</getcontentlength>"
    "</prop></propstat></response>"
    "<response><href>/sub</href><propstat><prop><resourcetype><collection/></resourcetype></prop></propstat></response>"
    "<response><href>/sub/deep.pdf</href><propstat><prop><getcontentlength>1</getcontentlength></prop></propstat></response>"
    "</multistatus>";

static void test_propfind(void)
{
    remote_entries e = { NULL, 0, 0 };
    int n = webdav_parse(nextcloud, strlen(nextcloud), "/remote.php/dav/files/ivan/Книги", &e);
    CHECK(n == 3, "nextcloud: %d элементов, ждали 3", n);
    const remote_entry *f = find(&e, "Война и мир.fb2");
    CHECK(f && !f->is_dir && f->size == 1234567, "файл с кириллицей и пробелами");
    CHECK(f && f->mtime == 1793606401LL, "время файла %lld", f ? f->mtime : -1);
    const remote_entry *d = find(&e, "Фантастика");
    CHECK(d && d->is_dir, "папка");
    const remote_entry *t = find(&e, "Tom & Jerry.pdf");
    CHECK(t && t->size == 42 && t->mtime == 0, "&amp; в имени, размер без времени");
    remote_entries_free(&e);

    n = webdav_parse(apache, strlen(apache), "/dav/books", &e);
    CHECK(n == 1, "apache: %d", n);
    CHECK(find(&e, "A&B.epub") && find(&e, "A&B.epub")->size == 10, "apache: A&B.epub");
    CHECK(find(&e, "A&B.epub") && find(&e, "A&B.epub")->mtime == 946684800LL, "apache: время");
    remote_entries_free(&e);

    n = webdav_parse(bare, strlen(bare), "", &e);
    CHECK(n == 2, "без префикса: %d", n);
    CHECK(find(&e, "a b.txt") && find(&e, "a b.txt")->size == 7, "CDATA и пробел");
    CHECK(find(&e, "sub") && find(&e, "sub")->is_dir, "папка без префикса");
    CHECK(!find(&e, "deep.pdf"), "глубже Depth: 1 не берётся");
    remote_entries_free(&e);

    /* сервер за прокси отдаёт пути иначе: первая — сама папка, остальные — дети */
    n = webdav_parse(nextcloud, strlen(nextcloud), "/other/prefix", &e);
    CHECK(n == 3 && find(&e, "Война и мир.fb2"), "запасной разбор по последней части пути: %d", n);
    remote_entries_free(&e);

    n = webdav_parse("<html><body>Hello</body></html>", 31, "/", &e);
    CHECK(n == -1, "не WebDAV: %d", n);
    remote_entries_free(&e);

    /* пустая папка: только она сама */
    const char *empty = "<d:multistatus xmlns:d=\"DAV:\"><d:response><d:href>/x/</d:href></d:response></d:multistatus>";
    n = webdav_parse(empty, strlen(empty), "/x", &e);
    CHECK(n == 0, "пустая папка: %d", n);
    remote_entries_free(&e);

    CHECK(webdav_parse_date("Tue, 03 Nov 2026 10:00:00 GMT") == 1793700000LL, "дата RFC 1123");
    CHECK(webdav_parse_date("2026-11-03T10:00:00Z") == 1793700000LL, "дата ISO");
    CHECK(webdav_parse_date("вчера") == 0, "мусор вместо даты");
}

/* ---- FTP ---- */

static void test_ftp(void)
{
    remote_entries e = { NULL, 0, 0 };
    const char *mlsd =
        "type=cdir;modify=20261124103000; .\r\n"
        "type=pdir;modify=20261124103000; ..\r\n"
        "type=file;size=1234;modify=20261124103000;perm=r; Война и мир.fb2\r\n"
        "Type=DIR;Modify=20261101000000;Perm=el; Фантастика\r\n"
        "type=file;size=0;modify=20200101000000.123; пустой.txt\r\n"
        "type=OS.unix=slink:/x;size=1; ссылка\r\n";
    int n = remote_parse_mlsd(mlsd, &e);
    CHECK(n == 3, "MLSD: %d", n);
    const remote_entry *f = find(&e, "Война и мир.fb2");
    CHECK(f && f->size == 1234 && f->mtime == 1795516200LL && !f->is_dir, "MLSD: файл (%lld)", f ? f->mtime : -1);
    CHECK(find(&e, "Фантастика") && find(&e, "Фантастика")->is_dir, "MLSD: папка");
    CHECK(find(&e, "пустой.txt") && find(&e, "пустой.txt")->size == 0, "MLSD: пустой файл");
    remote_entries_free(&e);

    const char *unix_list =
        "total 12\r\n"
        "drwxr-xr-x   2 ftp      ftp          4096 Nov 24 10:30 Фантастика\r\n"
        "-rw-r--r--   1 ftp      ftp       1234567 Nov 24 10:30 Война и мир.fb2\r\n"
        "-rw-r--r--   1 ftp      ftp            10 Jan  1  2020 two  spaces.pdf\r\n"
        "lrwxrwxrwx   1 ftp      ftp             5 Jan  1  2020 link -> target\r\n"
        "drwxr-xr-x   2 ftp      ftp          4096 Jan  1  2020 .\r\n"
        "drwxr-xr-x   2 ftp      ftp          4096 Jan  1  2020 ..\r\n"
        "-rw-r--r--   1 owner       999 Jan  1 10:00 nogroup.epub\r\n";
    n = remote_parse_list(unix_list, &e);
    CHECK(n == 4, "LIST unix: %d", n);
    CHECK(find(&e, "Фантастика") && find(&e, "Фантастика")->is_dir, "LIST: папка");
    CHECK(find(&e, "Война и мир.fb2") && find(&e, "Война и мир.fb2")->size == 1234567, "LIST: файл с пробелами");
    CHECK(find(&e, "two  spaces.pdf"), "LIST: два пробела внутри имени");
    CHECK(find(&e, "nogroup.epub") && find(&e, "nogroup.epub")->size == 999, "LIST: без группы");
    remote_entries_free(&e);

    const char *dos =
        "11-24-26  10:30AM       <DIR>          Фантастика\r\n"
        "11-24-2026  10:30AM              1234 Война и мир.fb2\r\n";
    n = remote_parse_list(dos, &e);
    CHECK(n == 2, "LIST DOS: %d", n);
    CHECK(find(&e, "Фантастика") && find(&e, "Фантастика")->is_dir, "DOS: папка");
    CHECK(find(&e, "Война и мир.fb2") && find(&e, "Война и мир.fb2")->size == 1234, "DOS: файл");
    remote_entries_free(&e);
}

/* ---- Адрес ---- */

static void test_url(void)
{
    char out[1024], err[160];
    CHECK(remote_normalize("  webdav.yandex.ru/Книги/ ", out, sizeof out, err, sizeof err) == 0, "https по умолчанию");
    CHECK(!strcmp(out, "https://webdav.yandex.ru/%D0%9A%D0%BD%D0%B8%D0%B3%D0%B8"), "кодирование: %s", out);
    CHECK(remote_normalize("https://h/a%20b/c d", out, sizeof out, err, sizeof err) == 0 &&
              !strcmp(out, "https://h/a%20b/c%20d"),
          "готовые %%XX остаются: %s", out);
    CHECK(remote_normalize("ftp://192.168.1.2:2121/books///", out, sizeof out, err, sizeof err) == 0 &&
              !strcmp(out, "ftp://192.168.1.2:2121/books"),
          "ftp: %s", out);
    CHECK(remote_normalize("https://host", out, sizeof out, err, sizeof err) == 0 && !strcmp(out, "https://host"),
          "корень");
    CHECK(remote_normalize("smb://host/x", out, sizeof out, err, sizeof err) != 0 && err[0], "чужая схема");
    CHECK(remote_normalize("   ", out, sizeof out, err, sizeof err) != 0, "пусто");
    CHECK(remote_normalize("https:///x", out, sizeof out, err, sizeof err) != 0, "нет хоста");
    /* неверные адреса отвергаются сразу, с понятным текстом */
    CHECK(remote_normalize("htt p//zzz", out, sizeof out, err, sizeof err) != 0 && strstr(err, "пробел"),
          "пробел в имени сервера: %s", err);
    CHECK(remote_normalize("https://my server/books", out, sizeof out, err, sizeof err) != 0 && strstr(err, "пробел"),
          "пробел в хосте со схемой: %s", err);
    CHECK(remote_normalize("https://host:80a/x", out, sizeof out, err, sizeof err) != 0 && strstr(err, "Порт"),
          "порт не число: %s", err);
    CHECK(remote_normalize("https://ho<st/x", out, sizeof out, err, sizeof err) != 0 && err[0], "знак в имени");
    CHECK(remote_normalize("https://:80/x", out, sizeof out, err, sizeof err) != 0, "только порт");
    CHECK(remote_normalize("https://u:pw@nas.local:8443/books", out, sizeof out, err, sizeof err) == 0 &&
              !strcmp(out, "https://u:pw@nas.local:8443/books"),
          "логин:пароль@хост:порт годится: %s", out);
    CHECK(remote_normalize("http://[::1]:8080/x", out, sizeof out, err, sizeof err) == 0, "IPv6: %s", err);
    CHECK(remote_normalize("http://192.168.0.5:8080/пробел в пути", out, sizeof out, err, sizeof err) == 0,
          "пробел в пути кодируется, не ошибка");
    remote_display("https://user:pw@webdav.yandex.ru/%D0%9A%D0%BD", out, sizeof out);
    CHECK(!strcmp(out, "webdav.yandex.ru/Кн"), "для показа: %s", out);
}

/* ---- Слияние ---- */

static char dir[256];

static void write_text(const char *name, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void path_of(const char *name, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", dir, name);
}

static void touch(const char *rel)
{
    char p[600];
    snprintf(p, sizeof p, "%s/books/%s", dir, rel);
    for (char *s = p + strlen(dir) + 7; (s = strchr(s, '/')) != NULL; s++) {
        *s = 0;
        mkdir(p, 0755);
        *s = '/';
    }
    FILE *f = fopen(p, "w");
    if (f) {
        fputs("x", f);
        fclose(f);
    }
}

static int read_all(const char *name, char *out, size_t len)
{
    char p[512];
    path_of(name, p, sizeof p);
    FILE *f = fopen(p, "r");
    size_t n = 0;
    if (f) {
        n = fread(out, 1, len - 1, f);
        fclose(f);
    }
    out[n] = 0;
    return f != NULL;
}

typedef struct {
    notes_files f;
    char sync[300], srv_pos[300], srv_bm[300], loc_bm[300], base[300], up_pos[300], up_bm[300], new_base[300];
    char pos_file[300];
} env;

static void env_init(env *e)
{
    snprintf(e->sync, sizeof e->sync, "%s/books", dir);
    path_of("srv-pos.txt", e->srv_pos, sizeof e->srv_pos);
    path_of("srv-bm.txt", e->srv_bm, sizeof e->srv_bm);
    path_of("loc-bm.txt", e->loc_bm, sizeof e->loc_bm);
    path_of("base.txt", e->base, sizeof e->base);
    path_of("up-pos.txt", e->up_pos, sizeof e->up_pos);
    path_of("up-bm.txt", e->up_bm, sizeof e->up_bm);
    path_of("new-base.txt", e->new_base, sizeof e->new_base);
    path_of("pos.txt", e->pos_file, sizeof e->pos_file);
    e->f = (notes_files){ e->sync, e->srv_pos, e->srv_bm, e->loc_bm, e->base, e->up_pos, e->up_bm, e->new_base };
}

/* строка positions.txt: страница с 1, всего, ключ */
static void put_pos(char *buf, size_t len, int page, int count, int finished, const char *path)
{
    size_t n = strlen(buf);
    snprintf(buf + n, len - n, "%d\t%d\t0\t-\to=0,s=0,c=0,f=%d\t%s\n", page, count, finished, path);
}

static void test_merge(void)
{
    snprintf(dir, sizeof dir, "/tmp/sync-test-XXXXXX");
    if (!mkdtemp(dir)) {
        printf("не создать каталог\n");
        fails++;
        return;
    }
    char b[300];
    snprintf(b, sizeof b, "%s/books", dir);
    mkdir(b, 0755);
    touch("a.epub");
    touch("Серия/b.fb2");
    touch("c.pdf");
    touch("d.pdf");
    env e;
    env_init(&e);

    /* ---- Первый раз, без базы: дальняя страница; свои остаются; серверные для книг, что есть — добавляются ---- */
    char local[4096] = "", srv[4096] = "";
    put_pos(local, sizeof local, 10, 100, 0, "/tmp/elsewhere.pdf"); /* вне папки синхронизации: абсолютный ключ */
    char ap[400], bp[400], cp[400];
    snprintf(ap, sizeof ap, "%s/books/a.epub", dir);
    snprintf(bp, sizeof bp, "%s/books/Серия/b.fb2", dir);
    snprintf(cp, sizeof cp, "%s/books/c.pdf", dir);
    put_pos(local, sizeof local, 5, 50, 0, ap);  /* на книге 5, на сервере 20 → 20 */
    put_pos(local, sizeof local, 30, 40, 0, bp); /* на книге 30, на сервере 7 → 30 */
    write_text("pos.txt", local);
    put_pos(srv, sizeof srv, 20, 50, 0, "a.epub");
    put_pos(srv, sizeof srv, 7, 40, 0, "Серия/b.fb2");
    put_pos(srv, sizeof srv, 12, 60, 1, "c.pdf");       /* только на сервере, книга на устройстве есть → добавить */
    put_pos(srv, sizeof srv, 3, 9, 0, "нет-такой.pdf"); /* книги нет → не добавлять, но сохранить */
    write_text("srv-pos.txt", srv);
    write_text("loc-bm.txt", "");
    write_text("srv-bm.txt", "");

    positions pos;
    CHECK(positions_load(&pos, e.pos_file) == 0 && pos.count == 3, "загрузка: %d", pos.count);
    notes_result r;
    CHECK(notes_merge(&pos, &e.f, &r) == 0, "слияние не вышло");
    CHECK(r.pos_updated == 1 && r.pos_imported == 1, "обновлено %d, добавлено %d", r.pos_updated, r.pos_imported);
    CHECK(r.send_pos == 1, "нужно отправить");
    const position *p = positions_find(&pos, ap);
    CHECK(p && p->page == 19, "a.epub: стр. %d (ждали 20)", p ? p->page + 1 : -1);
    p = positions_find(&pos, bp);
    CHECK(p && p->page == 29, "b.fb2 осталась на 30");
    p = positions_find(&pos, cp);
    CHECK(p && p->page == 11 && p->finished && p->count == 60, "c.pdf добавлена: дочитана");
    CHECK(positions_find(&pos, "/tmp/elsewhere.pdf") != NULL, "чужая книга на месте");
    CHECK(pos.count == 4, "в списке %d", pos.count);
    /* перезагрузка из файла: записано */
    positions again;
    positions_load(&again, e.pos_file);
    CHECK(again.count == 4, "записано %d", again.count);
    positions_free(&again);
    /* слитое для сервера: ключи без пути, чужая запись сохранена, абсолютный ключ — как есть */
    char text[8192];
    read_all("up-pos.txt", text, sizeof text);
    CHECK(strstr(text, "\ta.epub\n") && strstr(text, "\tСерия/b.fb2\n") && strstr(text, "\tc.pdf\n"), "ключи: %s", text);
    CHECK(strstr(text, "\tнет-такой.pdf\n") && strstr(text, "\t/tmp/elsewhere.pdf\n"), "чужие записи сохранены");
    CHECK(strstr(text, "20\t50\t") && strstr(text, "30\t40\t"), "страницы слитого");
    char base_text[4096];
    read_all("new-base.txt", base_text, sizeof base_text);
    CHECK(strstr(base_text, "p\t20\t0\ta.epub") && strstr(base_text, "p\t30\t0\tСерия/b.fb2"), "база: %s", base_text);
    rename(e.new_base, e.base);

    /* ---- Вторая синхронизация: базы есть; на книге читали дальше a.epub, на сервере — b.fb2 назад ---- */
    positions_set(&pos, ap, 24, 50);       /* на книге 25 (было 20) */
    positions_set(&pos, cp, 11, 60);       /* не менялась */
    positions_save(&pos);
    srv[0] = 0;
    put_pos(srv, sizeof srv, 20, 50, 0, "a.epub");        /* не менялась */
    put_pos(srv, sizeof srv, 8, 40, 0, "Серия/b.fb2");    /* на сервере вернулись назад: 8 < 30, но изменилась */
    put_pos(srv, sizeof srv, 12, 60, 0, "c.pdf");         /* «дочитана» сняли на сервере */
    put_pos(srv, sizeof srv, 3, 9, 0, "нет-такой.pdf");
    write_text("srv-pos.txt", srv);
    CHECK(notes_merge(&pos, &e.f, &r) == 0, "слияние 2");
    p = positions_find(&pos, ap);
    CHECK(p && p->page == 24, "a.epub: на книге изменили — остаётся 25, а не %d", p ? p->page + 1 : -1);
    p = positions_find(&pos, bp);
    CHECK(p && p->page == 7, "b.fb2: на сервере изменили (вернулись) — 8, а не %d", p ? p->page + 1 : -1);
    p = positions_find(&pos, cp);
    CHECK(p && p->finished == 0, "c.pdf: сняли «дочитана» на сервере");
    read_all("up-pos.txt", text, sizeof text);
    CHECK(strstr(text, "25\t50\t"), "наверх уходит 25");
    rename(e.new_base, e.base);

    /* ---- Ничего не менялось: отправлять нечего ---- */
    srv[0] = 0;
    read_all("up-pos.txt", srv, sizeof srv);
    write_text("srv-pos.txt", srv);
    CHECK(notes_merge(&pos, &e.f, &r) == 0 && r.send_pos == 0 && r.pos_updated == 0, "тишина: отправить %d, обновлено %d",
          r.send_pos, r.pos_updated);
    rename(e.new_base, e.base);
    positions_free(&pos);

    /* ---- Закладки ---- */
    char lb[2048], sb[2048];
    snprintf(lb, sizeof lb, "1,5,9\t%s\n3\t/tmp/elsewhere.pdf\n", ap);
    write_text("loc-bm.txt", lb);
    snprintf(sb, sizeof sb, "5,20\ta.epub\n2\tСерия/b.fb2\n7\tнет-такой.pdf\n");
    write_text("srv-bm.txt", sb);
    write_text("base.txt", "");
    positions_load(&pos, e.pos_file);
    CHECK(notes_merge(&pos, &e.f, &r) == 0, "слияние закладок");
    read_all("loc-bm.txt", text, sizeof text);
    char want[512];
    snprintf(want, sizeof want, "1,5,9,20\t%s\n", ap);
    CHECK(strstr(text, want), "книга: объединение 1,5,9,20: %s", text);
    CHECK(strstr(text, "\t/tmp/elsewhere.pdf\n"), "чужая закладка осталась");
    snprintf(want, sizeof want, "2\t%s\n", bp);
    CHECK(strstr(text, want), "закладка b.fb2 пришла с сервера");
    CHECK(!strstr(text, "нет-такой"), "для чужой книги на книге ничего не появляется");
    read_all("up-bm.txt", text, sizeof text);
    CHECK(strstr(text, "1,5,9,20\ta.epub\n") && strstr(text, "7\tнет-такой.pdf\n"), "наверх: %s", text);
    CHECK(r.send_bm == 1 && r.bm_updated == 2, "закладки: отправить %d, обновлено %d", r.send_bm, r.bm_updated);
    rename(e.new_base, e.base);

    /* снятая закладка: на книге сняли 9, на сервере сняли 20; объединение без них */
    snprintf(lb, sizeof lb, "1,5,20\t%s\n3\t/tmp/elsewhere.pdf\n2\t%s\n", ap, bp);
    write_text("loc-bm.txt", lb);
    write_text("srv-bm.txt", "1,5,9\ta.epub\n2\tСерия/b.fb2\n7\tнет-такой.pdf\n");
    CHECK(notes_merge(&pos, &e.f, &r) == 0, "слияние закладок 2");
    read_all("up-bm.txt", text, sizeof text);
    CHECK(strstr(text, "1,5\ta.epub\n"), "снятые закладки сняты с обеих сторон: %s", text);
    read_all("loc-bm.txt", text, sizeof text);
    snprintf(want, sizeof want, "1,5\t%s\n", ap);
    CHECK(strstr(text, want), "на книге то же: %s", text);
    positions_free(&pos);

    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        printf("не убрать %s\n", dir);
}

static void test_books(void)
{
    CHECK(sync_is_book("a.PDF") && sync_is_book("Война.fb2.zip") && sync_is_book("x.epub") && sync_is_book("x.docx"),
          "книжные форматы");
    CHECK(!sync_is_book("readme.md") && !sync_is_book("pdf") && !sync_is_book(".pdf") && !sync_is_book("a.pdf.part"),
          "не книги");
}

int main(void)
{
    test_books();
    test_propfind();
    test_ftp();
    test_url();
    test_merge();
    if (fails)
        printf("ОШИБОК: %d\n", fails);
    else
        printf("sync: всё хорошо\n");
    return fails ? 1 : 0;
}
