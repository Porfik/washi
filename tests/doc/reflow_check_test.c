/* reflow_check: потоковый счётчик вложенности и предпроверка файлов/EPUB до MuPDF. */
#include "../../src/doc/reflow_check.h"

#include <sys/stat.h>

#include "testutil.h"

static int depth_of(const char *html)
{
    reflow_scan s;
    reflow_scan_init(&s);
    reflow_scan_feed(&s, (const unsigned char *)html, strlen(html));
    return s.max_depth;
}

int main(void)
{
    /* счётчик */
    CHECK(depth_of("<html><body><div><p>a<p>b</div></body></html>") == 1, "p и html/body не считаются");
    CHECK(depth_of("<div><div><div></div></div></div>") == 3, "три div");
    CHECK(depth_of("<div><br><img src='a>b'/><span/></div>") == 1, "void, самозакрытые, '>' в кавычках");
    CHECK(depth_of("<!-- <div><div> --><div>") == 1, "комментарий");
    CHECK(depth_of("<script>var a='<div><div><div>';</script><div>") == 1, "внутри script тегов нет");
    CHECK(depth_of("<DIV><Div></DIV>") == 2, "регистр");
    CHECK(depth_of("a < b <div>") == 1, "'<' в тексте");
    /* граница между кусками */
    {
        reflow_scan s;
        const char *a = "<di", *b = "v><di", *c = "v>";
        reflow_scan_init(&s);
        reflow_scan_feed(&s, (const unsigned char *)a, strlen(a));
        reflow_scan_feed(&s, (const unsigned char *)b, strlen(b));
        reflow_scan_feed(&s, (const unsigned char *)c, strlen(c));
        CHECK(s.max_depth == 2, "теги через границу кусков: %d", s.max_depth);
    }

    {
        reflow_scan s;
        reflow_scan_init(&s);
        for (int i = 0; i < 10; i++)
            reflow_scan_feed(&s, (const unsigned char *)"<tr><td>x</td></tr>", 19);
        CHECK(s.rows == 10, "строки таблицы: %d", s.rows);
    }

    char dir[] = "/tmp/reflow-check-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char path[256], err[256];

    size_t len;
    char *ok = nested_html(200, &len);
    snprintf(path, sizeof path, "%s/ok.html", dir);
    write_file(path, ok, len);
    CHECK(reflow_check(path, err, sizeof err) == 0, "обычный html: %s", err);
    free(ok);

    char *bad = nested_html(200000, &len);
    snprintf(path, sizeof path, "%s/bad.html", dir);
    write_file(path, bad, len);
    err[0] = 0;
    CHECK(reflow_check(path, err, sizeof err) == -1 && err[0], "вложенность 200000 должна отклоняться");
    printf("причина: %s\n", err);

    snprintf(path, sizeof path, "%s/bad.fb2", dir);
    write_file(path, bad, len);
    CHECK(reflow_check(path, err, sizeof err) == -1, "fb2 с вложенностью");

    snprintf(path, sizeof path, "%s/bad.epub", dir);
    make_epub(path, bad, len);
    err[0] = 0;
    CHECK(reflow_check(path, err, sizeof err) == -1 && err[0], "epub с вложенностью в главе");

    char *fine = nested_html(100, &len);
    snprintf(path, sizeof path, "%s/ok.epub", dir);
    make_epub(path, fine, len);
    CHECK(reflow_check(path, err, sizeof err) == 0, "обычный epub: %s", err);
    free(fine);
    free(bad);

    /* не архив и пустой файл — не наше дело */
    snprintf(path, sizeof path, "%s/junk.epub", dir);
    write_file(path, "garbage", 7);
    CHECK(reflow_check(path, err, sizeof err) == 0, "мусорный epub пропускается");
    snprintf(path, sizeof path, "%s/missing.epub", dir);
    CHECK(reflow_check(path, err, sizeof err) == 0, "нет файла пропускается");

    /* таблица на 60 000 строк */
    {
        size_t cap = 60000 * 30;
        char *buf = malloc(cap), *p = buf;
        p += sprintf(p, "<html><body><table>");
        for (int i = 0; i < 60000; i++)
            p += sprintf(p, "<tr><td>x</td></tr>");
        snprintf(path, sizeof path, "%s/rows.html", dir);
        write_file(path, buf, (size_t)(p - buf));
        err[0] = 0;
        CHECK(reflow_check(path, err, sizeof err) == -1 && err[0], "таблица на 60000 строк");
        free(buf);
    }

    /* гигантский текст */
    {
        size_t big = REFLOW_MAX_FILE + 1;
        char *buf = malloc(big);
        memset(buf, 'a', big);
        snprintf(path, sizeof path, "%s/huge.html", dir);
        write_file(path, buf, big);
        err[0] = 0;
        CHECK(reflow_check(path, err, sizeof err) == -1 && err[0], "файл больше предела");
        free(buf);
    }

    char cmd[300];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "не удалось убрать %s\n", dir);
    if (failures) {
        fprintf(stderr, "%d ошибок\n", failures);
        return 1;
    }
    printf("reflow_check_test: ok\n");
    return 0;
}
