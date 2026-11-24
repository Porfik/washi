/* Настройки: текстовый файл «ключ=значение», строки с # — комментарии. */
#ifndef WASHI_BASE_CONFIG_H
#define WASHI_BASE_CONFIG_H

#define CONFIG_MAX_ENTRIES 64

typedef struct {
    int count;
    struct {
        char key[64];
        char value[512];
    } entries[CONFIG_MAX_ENTRIES];
} config;

/* Возвращает 0 или -1, если файла нет (config тогда пустой). */
int config_load(config *c, const char *path);

const char *config_get(const config *c, const char *key, const char *def);
int config_get_int(const config *c, const char *key, int def);

/* Меняет «ключ=значение» в файле path: первую строку с этим ключом (комментарии и порядок
 * остальных строк остаются), а если ключа нет — дописывает в конец. 0 — записано. */
int config_save_value(const char *path, const char *key, const char *value);

#endif
