/* cred — логины и пароли в файлах настроек: только зашифрованными (см. secret.h).
 * Логины шифруются тоже (это приватное), адреса — нет. */
#ifndef WASHI_BASE_CRED_H
#define WASHI_BASE_CRED_H

#include <stddef.h>
#include <stdio.h>

#include "config.h"

enum {
    CRED_OK = 0,
    CRED_OLD = 1,  /* прочитано открытое (старый файл): файл стоит переписать */
    CRED_BAD = -1, /* не расшифровалось: значение пустое, ввести заново */
};

/* Зашифровать для файла. 0 — out = «enc1:…» (пустое остаётся пустым); -1 — не вышло,
 * out пустая (открытым текстом не пишем никогда). */
int cred_pack(const char *plain, char *out, size_t len);
/* Прочитанное из файла → открытый текст. Результат CRED_*. */
int cred_unpack(const char *stored, char *out, size_t len);

/* Поле «ключ=значение» в настройках: прочитать / дописать в файл. */
int cred_config_get(const config *c, const char *key, char *out, size_t len);
void cred_config_put(FILE *f, const char *key, const char *plain);

/* Свои каталоги OPDS: название, адрес, логин, пароль (через табуляцию). */
typedef struct {
    char name[96];
    char url[512];
    char login[96], pass[96];
    int lost; /* вход не расшифровался (логин и пароль пусты) */
} cred_cat;

/* Читает до max каталогов. Возвращает их число; *rewrite = 1, если в файле было открытое
 * значение (нужно сохранить заново). */
int cred_cats_load(const char *path, cred_cat *out, int max, int *rewrite);
int cred_cats_save(const char *path, const cred_cat *c, int n);

#endif
