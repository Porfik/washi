/* cred: логины и пароли в файлах — не открытым текстом; старое открытое читается и переписывается;
 * испорченное — пусто, без падения. */
#include "../../src/base/cred.h"
#include "../../src/base/secret.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

const char *pf_data_dir(void)
{
    return "/tmp";
}

int pf_device_secret(char *buf, size_t len)
{
    snprintf(buf, len, "test-device|SN123");
    return 0;
}

static int fails;
#define CHECK(c, what) do { if (!(c)) { printf("НЕ ТАК (строка %d): %s\n", __LINE__, what); fails++; } } while (0)

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return strdup("");
    char *b = calloc(1, 1 << 16);
    size_t n = fread(b, 1, (1 << 16) - 1, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    fputs(text, f);
    fclose(f);
}

static void test_cats(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/opds.txt", dir);
    cred_cat c[3];
    memset(c, 0, sizeof c);
    snprintf(c[0].name, sizeof c[0].name, "Домашний");
    snprintf(c[0].url, sizeof c[0].url, "http://home.lan/opds");
    snprintf(c[0].login, sizeof c[0].login, "иван-читатель");
    snprintf(c[0].pass, sizeof c[0].pass, "S3kret пароль!");
    snprintf(c[1].name, sizeof c[1].name, "Открытый");
    snprintf(c[1].url, sizeof c[1].url, "https://open.example/opds");
    CHECK(cred_cats_save(path, c, 2) == 0, "save");
    char *txt = slurp(path);
    CHECK(!strstr(txt, "иван-читатель") && !strstr(txt, "S3kret") && !strstr(txt, "пароль!"), "в файле нет логина и пароля открытым текстом");
    CHECK(strstr(txt, "http://home.lan/opds") != NULL, "адрес открыт");
    CHECK(strstr(txt, "enc1:") != NULL, "зашифровано");
    free(txt);
    cred_cat back[4];
    int rw = 9;
    int n = cred_cats_load(path, back, 4, &rw);
    CHECK(n == 2 && rw == 0, "прочитано без переписывания");
    CHECK(!strcmp(back[0].login, "иван-читатель") && !strcmp(back[0].pass, "S3kret пароль!"), "вход вернулся");
    CHECK(!back[1].login[0] && !back[1].pass[0] && !back[1].lost, "без входа — пусто");

    /* старый открытый файл */
    put(path, "# старое\nДомашний\thttp://home.lan/opds\tivan\tplainpass\nДругой\thttp://x/opds\t\t\n");
    n = cred_cats_load(path, back, 4, &rw);
    CHECK(n == 2 && rw == 1, "открытое — нужно переписать");
    CHECK(!strcmp(back[0].login, "ivan") && !strcmp(back[0].pass, "plainpass"), "открытое прочиталось");
    CHECK(cred_cats_save(path, back, n) == 0, "перезапись");
    txt = slurp(path);
    CHECK(!strstr(txt, "ivan") && !strstr(txt, "plainpass"), "после перезаписи открытого нет");
    free(txt);
    n = cred_cats_load(path, back, 4, &rw);
    CHECK(n == 2 && rw == 0 && !strcmp(back[0].pass, "plainpass"), "после перезаписи читается");

    /* испорченное значение */
    txt = slurp(path);
    char *p = strstr(txt, "enc1:");
    CHECK(p != NULL, "есть что портить");
    if (p) {
        p[12] = p[12] == 'A' ? 'B' : 'A';
        p[13] = p[13] == 'A' ? 'B' : 'A';
    }
    put(path, txt);
    free(txt);
    n = cred_cats_load(path, back, 4, &rw);
    CHECK(n == 2, "каталоги на месте");
    CHECK(!back[0].login[0] && !back[0].pass[0] && back[0].lost && rw == 1, "испорченный вход — пусто, пара целиком");
    CHECK(!strcmp(back[0].url, "http://home.lan/opds"), "адрес цел");
    put(path, "A\thttp://a/\tenc1:не-base64\tenc1:\n");
    n = cred_cats_load(path, back, 4, &rw);
    CHECK(n == 1 && back[0].lost && !back[0].pass[0], "мусор не роняет");
}

static void test_config(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/sync.txt", dir);
    FILE *f = fopen(path, "w");
    fprintf(f, "url=https://dav.example/\n");
    cred_config_put(f, "login", "user@mail.example");
    cred_config_put(f, "password", "пароль приложения=с равно");
    fclose(f);
    char *txt = slurp(path);
    CHECK(!strstr(txt, "user@mail") && !strstr(txt, "пароль приложения"), "sync.txt: открытого нет");
    free(txt);
    config c;
    config_load(&c, path);
    char lg[128], pw[128];
    CHECK(cred_config_get(&c, "login", lg, sizeof lg) == CRED_OK && !strcmp(lg, "user@mail.example"), "login");
    CHECK(cred_config_get(&c, "password", pw, sizeof pw) == CRED_OK && !strcmp(pw, "пароль приложения=с равно"), "password");
    CHECK(cred_config_get(&c, "нет-такого", pw, sizeof pw) == CRED_OK && !pw[0], "нет ключа — пусто");

    put(path, "login=oldlogin\npassword=oldpass\n");
    config_load(&c, path);
    CHECK(cred_config_get(&c, "login", lg, sizeof lg) == CRED_OLD && !strcmp(lg, "oldlogin"), "старое открытое: читается, помечено");

    put(path, "login=enc1:AAAA\npassword=enc1:\n");
    config_load(&c, path);
    memset(pw, 'x', sizeof pw - 1);
    pw[sizeof pw - 1] = 0;
    CHECK(cred_config_get(&c, "login", lg, sizeof lg) == CRED_BAD && !lg[0], "испорченное — пусто");
    CHECK(cred_config_get(&c, "password", pw, sizeof pw) == CRED_BAD && !pw[0], "пустое enc1 — пусто");
}

int main(void)
{
    secret_init();
    char dir[] = "/tmp/cred-test-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    test_cats(dir);
    test_config(dir);
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd))
        return 2;
    printf(fails ? "cred: %d ошибок\n" : "cred: всё верно\n", fails);
    return fails ? 1 : 0;
}
