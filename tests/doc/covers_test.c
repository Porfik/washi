/* Обложки: «злой» файл не вешает поток и не роняет; метка .try пропускает книгу после обрыва. */
#include "../../src/doc/covers.c" /* внутренние make_key/метка */

#include <sys/stat.h>
#include <unistd.h>

#include "testutil.h"

static int wait_state(covers *c, const char *path, int want, int ms, const cover_info **ci)
{
    double t0 = clock_ms();
    int rc;
    do {
        rc = covers_get(c, path, ci);
        if (rc != 1)
            return rc;
        usleep(20000);
    } while (clock_ms() - t0 < ms);
    return rc == want ? rc : 99;
}

int main(void)
{
    char dir[] = "/tmp/covers-test-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char cdir[300], bad[300], good[300], marked[300], junk[300], mark[400];
    snprintf(cdir, sizeof cdir, "%s/covers", dir);
    snprintf(bad, sizeof bad, "%s/evil.html", dir);
    snprintf(good, sizeof good, "%s/good.html", dir);
    snprintf(marked, sizeof marked, "%s/marked.html", dir);
    snprintf(junk, sizeof junk, "%s/junk.epub", dir);

    size_t len;
    char *evil = nested_html(300000, &len); /* без предпроверки это минуты и часы */
    write_file(bad, evil, len);
    free(evil);
    write_file(good, "<html><body><h1>Title</h1><p>Hello</p></body></html>", 52);
    write_file(marked, "<html><body><p>Hello</p></body></html>", 39);
    write_file(junk, "not a zip", 9);

    doc_options opt;
    memset(&opt, 0, sizeof opt);
    opt.store_bytes = 16 << 20;
    opt.no_icc = 1;
    covers *c = covers_new(cdir, &opt);
    CHECK(c != NULL, "covers_new");
    if (!c)
        return 1;

    const cover_info *ci = NULL;
    double t0 = clock_ms();
    int rc = wait_state(c, bad, -1, 10000, &ci);
    CHECK(rc == -1, "злой файл: ожидали отказ (-1), получили %d", rc);
    CHECK(clock_ms() - t0 < 5000, "злой файл разобран за %.0f мс", clock_ms() - t0);
    CHECK(wait_state(c, junk, -1, 5000, &ci) == -1, "мусорный epub — отказ");
    rc = wait_state(c, good, 0, 10000, &ci);
    CHECK(rc == 0, "нормальный html: обложка (%d)", rc);

    /* метка прошлой неудачной попытки */
    struct stat st;
    CHECK(stat(marked, &st) == 0, "stat");
    char key[17];
    make_key(marked, &st, key);
    snprintf(mark, sizeof mark, "%s/%s.try", cdir, key);
    write_file(mark, "", 0);
    CHECK(covers_get(c, marked, &ci) == -1, "с меткой .try книга пропускается сразу");
    usleep(300000);
    CHECK(covers_get(c, marked, &ci) == -1, "и остаётся пропущенной");
    CHECK(access(mark, F_OK) == 0, "метка не стирается без новой попытки");

    /* нормальная работа не оставляет метки */
    snprintf(mark, sizeof mark, "%s/%s.try", cdir, "*");
    char cmd[600];
    snprintf(cmd, sizeof cmd, "ls %s/*.try 2>/dev/null | wc -l", cdir);
    FILE *p = popen(cmd, "r");
    int n = -1;
    if (p) {
        if (fscanf(p, "%d", &n) != 1)
            n = -1;
        pclose(p);
    }
    CHECK(n == 1, "осталась только заранее созданная метка, а их %d", n);

    covers_free(c);
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "не удалось убрать %s\n", dir);
    if (failures) {
        fprintf(stderr, "%d ошибок\n", failures);
        return 1;
    }
    printf("covers_test: ok\n");
    return 0;
}
