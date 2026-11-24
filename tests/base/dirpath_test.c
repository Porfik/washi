/* Пути папок внутри библиотеки (dirpath.c): присоединение, обратно, список подпапок, выбор «своей папки». */
#define _GNU_SOURCE
#include "../../src/base/dirpath.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;

#define CHECK(cond, ...)                                                                                          \
    do {                                                                                                          \
        if (!(cond)) {                                                                                            \
            printf("НЕ ТАК (строка %d): ", __LINE__);                                                             \
            printf(__VA_ARGS__);                                                                                  \
            printf("\n");                                                                                         \
            fails++;                                                                                              \
        }                                                                                                         \
    } while (0)

static void join_cases(void)
{
    char o[256];
    CHECK(dirpath_join("/mnt/ext1", "Downloads", o, sizeof o) == 0 && !strcmp(o, "/mnt/ext1/Downloads"), "%s", o);
    CHECK(dirpath_join("/mnt/ext1/", "", o, sizeof o) == 0 && !strcmp(o, "/mnt/ext1"), "%s", o);
    CHECK(dirpath_join("/mnt/ext1", ".", o, sizeof o) == 0 && !strcmp(o, "/mnt/ext1"), "%s", o);
    CHECK(dirpath_join("/mnt/ext1", "//A///./B/", o, sizeof o) == 0 && !strcmp(o, "/mnt/ext1/A/B"), "%s", o);
    CHECK(dirpath_join("/mnt/ext1", "../etc", o, sizeof o) != 0, "«..» должна отклоняться");
    CHECK(dirpath_join("/mnt/ext1", "A/../../x", o, sizeof o) != 0, "«..» внутри должна отклоняться");
    CHECK(dirpath_join("/mnt/ext1", "A\\B", o, sizeof o) != 0, "обратная косая");
    CHECK(dirpath_join("/mnt/ext1", "/etc", o, sizeof o) == 0 && !strcmp(o, "/mnt/ext1/etc"), "абсолютный → внутри корня: %s", o);
    CHECK(dirpath_join("/mnt/ext1", "Книги/Новые", o, sizeof o) == 0 && !strcmp(o, "/mnt/ext1/Книги/Новые"), "%s", o);
    char tiny[8];
    CHECK(dirpath_join("/mnt/ext1", "Downloads", tiny, sizeof tiny) != 0, "не влезло — отказ");
}

static void relative_cases(void)
{
    char r[256];
    CHECK(dirpath_relative("/mnt/ext1", "/mnt/ext1/Downloads", r, sizeof r) == 0 && !strcmp(r, "Downloads"), "%s", r);
    CHECK(dirpath_relative("/mnt/ext1", "/mnt/ext1", r, sizeof r) == 0 && !*r, "корень → пусто");
    CHECK(dirpath_relative("/mnt/ext1/", "/mnt/ext1/A//B/", r, sizeof r) == 0 && !strcmp(r, "A/B"), "%s", r);
    CHECK(dirpath_relative("/mnt/ext1", "/mnt/ext10/A", r, sizeof r) != 0, "соседняя папка с общим началом");
    CHECK(dirpath_relative("/mnt/ext1", "/etc", r, sizeof r) != 0, "вне корня");
    CHECK(dirpath_relative("/mnt/ext1", "/mnt/ext1/../x", r, sizeof r) != 0, "«..»");
}

static void config_cases(void)
{
    char o[256];
    CHECK(dirpath_config("/r", NULL, o, sizeof o) == 0 && !strcmp(o, "/r/Downloads"), "%s", o);
    CHECK(dirpath_config("/r", "", o, sizeof o) == 0 && !strcmp(o, "/r/Downloads"), "%s", o);
    CHECK(dirpath_config("/r", "Books/New", o, sizeof o) == 0 && !strcmp(o, "/r/Books/New"), "%s", o);
    CHECK(dirpath_config("/r", ".", o, sizeof o) == 0 && !strcmp(o, "/r"), "%s", o);
    CHECK(dirpath_config("/r", "/elsewhere/dl", o, sizeof o) == 0 && !strcmp(o, "/elsewhere/dl"), "%s", o);
    CHECK(dirpath_config("/r", "../x", o, sizeof o) != 0, "«..» в config.txt");
}

static void mk(const char *base, const char *rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", base, rel);
    if (mkdir(p, 0755) != 0)
        perror(p);
}

static void list_cases(const char *tmp)
{
    char root[512], outside[512];
    snprintf(root, sizeof root, "%s/lib", tmp);
    snprintf(outside, sizeof outside, "%s/outside", tmp);
    mkdir(root, 0755);
    mkdir(outside, 0755);
    mk(root, "beta");
    mk(root, "Alpha");
    mk(root, ".hidden");
    mk(root, "system");
    mk(root, "Applications");
    mk(root, "hubdata");
    mk(root, "Alpha/system"); /* не в корне — виден */
    mk(root, "Alpha/Inner");
    char p[1024], q[1024];
    snprintf(p, sizeof p, "%s/file.txt", root);
    FILE *f = fopen(p, "w");
    if (f)
        fclose(f);
    snprintf(p, sizeof p, "%s/link_in", root);
    snprintf(q, sizeof q, "%s/beta", root);
    CHECK(symlink(q, p) == 0, "symlink внутрь");
    snprintf(p, sizeof p, "%s/link_out", root);
    CHECK(symlink(outside, p) == 0, "symlink наружу");

    char **names;
    int n;
    char hide[600];
    snprintf(hide, sizeof hide, "%s/hubdata", root);
    CHECK(dirpath_subdirs(root, hide, "", &names, &n) == 0, "список корня");
    CHECK(n == 3 && !strcmp(names[0], "Alpha") && !strcmp(names[1], "beta") && !strcmp(names[2], "link_in"),
          "корень: %d: %s %s %s", n, n > 0 ? names[0] : "", n > 1 ? names[1] : "", n > 2 ? names[2] : "");
    dirpath_free(names, n);
    CHECK(dirpath_subdirs(root, hide, "Alpha", &names, &n) == 0 && n == 2 && !strcmp(names[0], "Inner") &&
              !strcmp(names[1], "system"),
          "Alpha: %d", n);
    dirpath_free(names, n);
    CHECK(dirpath_subdirs(root, hide, "../outside", &names, &n) != 0, "наружу — отказ");
    CHECK(dirpath_subdirs(root, hide, "нет", &names, &n) != 0, "нет папки — отказ");
}

static void choice_cases(const char *tmp)
{
    char file[512], rel[256];
    snprintf(file, sizeof file, "%s/recv_dir.txt", tmp);
    CHECK(dirpath_choice_load(file, rel, sizeof rel) == 0, "нет файла — общая");
    CHECK(dirpath_choice_save(file, "Книги/Новые") == 0, "сохранить");
    CHECK(dirpath_choice_load(file, rel, sizeof rel) == 1 && !strcmp(rel, "Книги/Новые"), "прочитать: %s", rel);
    CHECK(dirpath_choice_save(file, ".") == 0, "корень");
    CHECK(dirpath_choice_load(file, rel, sizeof rel) == 1 && !strcmp(rel, "."), "корень прочитан: %s", rel);
    CHECK(dirpath_choice_save(file, "") == 0 && access(file, F_OK) != 0, "пусто — файл удалён");
    CHECK(dirpath_choice_load(file, rel, sizeof rel) == 0, "после сброса — общая");
    FILE *f = fopen(file, "w");
    fputs("../../etc\n", f);
    fclose(f);
    CHECK(dirpath_choice_load(file, rel, sizeof rel) == 0, "«..» в файле — общая");
    unlink(file);
}

int main(void)
{
    char tmp[] = "/tmp/washi-dirpath-XXXXXX";
    if (!mkdtemp(tmp))
        return 2;
    join_cases();
    relative_cases();
    config_cases();
    list_cases(tmp);
    choice_cases(tmp);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmp);
    if (system(cmd) != 0)
        printf("не убрал %s\n", tmp);
    if (fails)
        printf("ПРОВАЛЕНО: %d\n", fails);
    else
        printf("dirpath: всё верно\n");
    return fails != 0;
}
