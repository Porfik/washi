#define _DEFAULT_SOURCE
/* bookfiles: переименование и удаление книг и папок — место чтения, закладки и обложка едут за книгой
 * или убираются; не затираем чужое, не выходим за корень библиотеки, не трогаем папки прошивки. */
#include "../../src/base/bookfiles.h"
#include "../../src/base/bookmarks.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../doc/testutil.h"

const char *pf_data_dir(void)
{
    return "/tmp";
}

static int exists(const char *p)
{
    return access(p, F_OK) == 0;
}

static const char *HTML = "<html><body><h1>Title</h1><p>Hello</p></body></html>";

static void book(const char *path)
{
    write_file(path, HTML, strlen(HTML));
}

static int cover_ready(covers *cv, const char *path)
{
    const cover_info *ci = NULL;
    for (int i = 0; i < 500; i++) {
        int rc = covers_get(cv, path, &ci);
        if (rc != 1)
            return rc == 0;
        usleep(20000);
    }
    return 0;
}

static int bm_count(const char *file, const char *path)
{
    bookmarks b;
    bookmarks_load(&b, file, path);
    return b.count;
}

int main(void)
{
    char dir[] = "/tmp/bookfiles-test-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char root[300], data[300], p[600], q[1100], bmf[400], posf[400], cdir[400], out[1100];
    snprintf(root, sizeof root, "%s/lib", dir);
    snprintf(data, sizeof data, "%s/data", dir);
    mkdir(root, 0755);
    mkdir(data, 0755);
    snprintf(bmf, sizeof bmf, "%s/bookmarks.txt", data);
    snprintf(posf, sizeof posf, "%s/positions.txt", data);
    snprintf(cdir, sizeof cdir, "%s/covers", data);

    doc_options opt;
    memset(&opt, 0, sizeof opt);
    opt.store_bytes = 16 << 20;
    opt.no_icc = 1;
    covers *cv = covers_new(cdir, &opt);
    CHECK(cv != NULL, "covers_new");

    positions pos;
    positions_load(&pos, posf);

    /* Лист: lib/Война и мир.html, lib/Sub/Глава.html, lib/Sub/Deep/Конец.html, lib/other.html, lib/system/x.html */
    char sub[400], deep[400], sysd[400];
    snprintf(sub, sizeof sub, "%s/Sub", root);
    snprintf(deep, sizeof deep, "%s/Sub/Deep", root);
    snprintf(sysd, sizeof sysd, "%s/system", root);
    mkdir(sub, 0755);
    mkdir(deep, 0755);
    mkdir(sysd, 0755);
    char b1[500], b2[500], b3[500], b4[500], b5[500];
    snprintf(b1, sizeof b1, "%s/Война и мир.html", root);
    snprintf(b2, sizeof b2, "%s/Sub/Глава.html", root);
    snprintf(b3, sizeof b3, "%s/Sub/Deep/Конец.html", root);
    snprintf(b4, sizeof b4, "%s/other.html", root);
    snprintf(b5, sizeof b5, "%s/system/x.html", root);
    book(b1);
    book(b2);
    book(b3);
    book(b4);
    book(b5);
    positions_set(&pos, b1, 41, 100);
    positions_set_finished(&pos, b1, 1);
    positions_set(&pos, b3, 6, 10);
    positions_set(&pos, b4, 1, 5);
    bookmarks bm;
    bookmarks_load(&bm, bmf, b1);
    bookmarks_toggle(&bm, 3);
    bookmarks_toggle(&bm, 9);
    bookmarks_load(&bm, bmf, b3);
    bookmarks_toggle(&bm, 4);
    bookmarks_load(&bm, bmf, b4);
    bookmarks_toggle(&bm, 7);

    CHECK(cover_ready(cv, b1), "обложка книги 1 нарисовалась");
    CHECK(cover_ready(cv, b3), "обложка книги 3 нарисовалась");

    /* --- Переименование книги --- */
    bf_result r = bookfiles_rename_book(root, &pos, bmf, cv, b1, "  Новое имя  ", out, sizeof out);
    snprintf(q, sizeof q, "%s/Новое имя.html", root);
    CHECK(r == BF_OK, "переименование: %d", r);
    CHECK(!strcmp(out, q), "новый путь: %s", out);
    CHECK(!exists(b1) && exists(q), "файл переехал");
    const position *ps = positions_find(&pos, q);
    CHECK(ps && ps->page == 41 && ps->count == 100 && ps->finished, "место чтения за новым именем");
    CHECK(!positions_find(&pos, b1), "старого имени в позициях нет");
    CHECK(bm_count(bmf, q) == 2 && bm_count(bmf, b1) == 0, "закладки переехали");
    positions again;
    positions_load(&again, posf);
    CHECK(positions_find(&again, q) != NULL, "позиции записаны на диск");
    positions_free(&again);
    CHECK(cover_ready(cv, q), "обложка у нового имени");
    {
        /* Обложка не рисовалась заново: сведения лежат в кэше под новым ключом. */
        struct stat st;
        stat(q, &st);
        int n = 0;
        char cmd[600];
        snprintf(cmd, sizeof cmd, "ls %s/*.inf 2>/dev/null | wc -l", cdir);
        FILE *f = popen(cmd, "r");
        if (f && fscanf(f, "%d", &n) != 1)
            n = -1;
        if (f)
            pclose(f);
        CHECK(n == 2, "в кэше обложек по одной записи на книгу (%d)", n);
    }

    /* Расширение дописали сами — не удваивается; то же имя — ничего. */
    r = bookfiles_rename_book(root, &pos, bmf, cv, q, "Новое имя.HTML", out, sizeof out);
    CHECK(r == BF_SAME, "то же имя с расширением: %d", r);
    r = bookfiles_rename_book(root, &pos, bmf, cv, q, "   ", out, sizeof out);
    CHECK(r == BF_INVALID, "пустое имя: %d", r);
    r = bookfiles_rename_book(root, &pos, bmf, cv, q, "other", out, sizeof out);
    CHECK(r == BF_EXISTS && exists(q) && exists(b4), "занятое имя не затирается: %d", r);
    r = bookfiles_rename_book(root, &pos, bmf, cv, q, "a/b:c?", out, sizeof out);
    snprintf(p, sizeof p, "%s/b_c_.html", root);
    CHECK(r == BF_OK && !strcmp(out, p) && exists(p), "небезопасные символы: %d %s", r, out);
    snprintf(q, sizeof q, "%s", out);

    /* --- Вне библиотеки и папки прошивки --- */
    char outside[500];
    snprintf(outside, sizeof outside, "%s/outside.html", dir);
    book(outside);
    r = bookfiles_delete_book(root, &pos, bmf, cv, outside);
    CHECK(r == BF_OUTSIDE && exists(outside), "книга вне корня не удаляется: %d", r);
    r = bookfiles_rename_book(root, &pos, bmf, cv, outside, "x", out, sizeof out);
    CHECK(r == BF_OUTSIDE && exists(outside), "и не переименовывается: %d", r);
    snprintf(p, sizeof p, "%s/../outside.html", root);
    r = bookfiles_delete_book(root, &pos, bmf, cv, p);
    CHECK(r == BF_OUTSIDE && exists(outside), "«..» не пускает: %d", r);
    r = bookfiles_delete_book(root, &pos, bmf, cv, b5);
    CHECK(r == BF_OUTSIDE && exists(b5), "папка system не трогается: %d", r);
    r = bookfiles_delete_dir(root, &pos, bmf, cv, sysd);
    CHECK(r == BF_OUTSIDE && exists(sysd), "папка system не удаляется: %d", r);
    r = bookfiles_delete_dir(root, &pos, bmf, cv, root);
    CHECK(r == BF_OUTSIDE && exists(root), "корень не удаляется: %d", r);
    r = bookfiles_rename_dir(root, &pos, bmf, cv, sub, "applications", out, sizeof out);
    CHECK(r == BF_OUTSIDE && exists(sub), "папку нельзя назвать как папку прошивки: %d", r);
    /* ссылка наружу: удаляется ссылка, а не цель */
    snprintf(p, sizeof p, "%s/link", root);
    CHECK(symlink(dir, p) == 0, "symlink");
    r = bookfiles_delete_dir(root, &pos, bmf, cv, p);
    CHECK(r == BF_OUTSIDE && exists(outside), "ссылка-папка наружу не пускает: %d", r);
    remove(p);
    snprintf(p, sizeof p, "%s/linkfile.html", root);
    CHECK(symlink(outside, p) == 0, "symlink");
    r = bookfiles_delete_book(root, &pos, bmf, cv, p);
    CHECK(r == BF_OK && !exists(p) && exists(outside), "ссылка на книгу: удалена ссылка, цель цела: %d", r);

    /* --- Переименование папки --- */
    r = bookfiles_rename_dir(root, &pos, bmf, cv, sub, "Подборка", out, sizeof out);
    char nsub[400], nb3[500];
    snprintf(nsub, sizeof nsub, "%s/Подборка", root);
    snprintf(nb3, sizeof nb3, "%s/Подборка/Deep/Конец.html", root);
    CHECK(r == BF_OK && !strcmp(out, nsub) && exists(nb3) && !exists(sub), "папка переименована: %d", r);
    ps = positions_find(&pos, nb3);
    CHECK(ps && ps->page == 6 && !positions_find(&pos, b3), "позиция книги в папке переехала");
    CHECK(bm_count(bmf, nb3) == 1 && bm_count(bmf, b3) == 0, "закладка книги в папке переехала");
    CHECK(cover_ready(cv, nb3), "обложка книги в папке");
    char nested[500];
    snprintf(nested, sizeof nested, "%s/Подборка/Deep", root);
    r = bookfiles_rename_dir(root, &pos, bmf, cv, nested, "system", out, sizeof out);
    CHECK(r == BF_OK, "вложенную папку можно назвать system: %d", r);

    /* --- Удаление книги --- */
    r = bookfiles_delete_book(root, &pos, bmf, cv, b4);
    CHECK(r == BF_OK && !exists(b4), "книга удалена: %d", r);
    CHECK(!positions_find(&pos, b4) && bm_count(bmf, b4) == 0, "место чтения и закладки удалены");
    positions_load(&again, posf);
    CHECK(!positions_find(&again, b4), "и на диске");
    positions_free(&again);
    r = bookfiles_delete_book(root, &pos, bmf, cv, b4);
    CHECK(r == BF_OUTSIDE, "повторно — нет такой: %d", r);

    /* --- Удаление папки --- */
    r = bookfiles_delete_dir(root, &pos, bmf, cv, nsub);
    CHECK(r == BF_OK && !exists(nsub), "папка удалена: %d", r);
    for (int i = 0; i < pos.count; i++)
        CHECK(strncmp(pos.items[i].path, nsub, strlen(nsub)), "в позициях остались книги удалённой папки: %s",
              pos.items[i].path);
    CHECK(bm_count(bmf, nb3) == 0, "закладки удалённой папки убраны");
    CHECK(exists(q) && exists(outside) && exists(b5), "остальное цело");

    positions_free(&pos);
    covers_free(cv);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "не стёрт %s\n", dir);
    if (failures)
        return 1;
    printf("ok\n");
    return 0;
}
