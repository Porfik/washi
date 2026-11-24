/* secret: пароль туда и обратно, русские буквы, открытое значение как есть, порча не проходит. */
#include "../../src/base/secret.h"

#include <stdio.h>
#include <string.h>

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
#define CHECK(c, what) do { if (!(c)) { printf("НЕ ТАК: %s\n", what); fails++; } } while (0)

int main(void)
{
    secret_init();
    const char *pw[] = { "p", "пароль приложения Яндекса", "0123456789abcdef", "with=equals\tand tab" };
    for (int i = 0; i < 4; i++) {
        char sealed[512], back[256];
        CHECK(secret_seal(pw[i], sealed, sizeof sealed) == 0, "seal");
        CHECK(secret_is_sealed(sealed), "is_sealed");
        CHECK(strlen(pw[i]) < 4 || !strstr(sealed, pw[i]), "не видно открытым текстом");
        CHECK(!strpbrk(sealed, "=\t\n |"), "нет разделителей файлов");
        CHECK(strlen(sealed) <= secret_sealed_len(strlen(pw[i])), "длина");
        CHECK(secret_open(sealed, back, sizeof back) == 0 && !strcmp(back, pw[i]), "open");
        char again[512];
        secret_seal(pw[i], again, sizeof again);
        CHECK(strcmp(again, sealed), "каждый раз разное (случайный вектор)");
        sealed[10] = sealed[10] == 'A' ? 'B' : 'A';
        CHECK(secret_open(sealed, back, sizeof back) == -1 && !back[0], "испорченное не открывается");
    }
    char out[64];
    CHECK(secret_open("old-plain", out, sizeof out) == 0 && !strcmp(out, "old-plain"), "открытое как есть");
    CHECK(secret_open("", out, sizeof out) == 0 && !out[0], "пустое");
    CHECK(secret_seal("", out, sizeof out) == 0 && !out[0], "пустое не шифруется");
    char tiny[8];
    CHECK(secret_seal("long password", tiny, sizeof tiny) == -1, "мало места");
    printf(fails ? "secret: %d ошибок\n" : "secret: всё верно\n", fails);
    return fails != 0;
}
