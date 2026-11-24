/* dirpath — пути папок внутри библиотеки (library_root) для выбора папки загрузок: «Downloads»,
 * «Книги/Новые». Всё лексически: «..» и пути наружу не принимаются; символические ссылки
 * в списке папок отсеиваются по realpath. Без интерфейса — проверяется тестом. */
#ifndef WASHI_BASE_DIRPATH_H
#define WASHI_BASE_DIRPATH_H

#include <stddef.h>

/* Путь rel (от корня root) → полный. "", "." — сам корень; лишние «/» и «.» убираются; «..»,
 * «\», управляющие символы — отказ. 0 — готово, -1 — нельзя или не влезло. */
int dirpath_join(const char *root, const char *rel, char *out, size_t len);

/* Полный путь abs внутри root → относительный (в rel: "" для самого корня). -1 — abs не внутри root. */
int dirpath_relative(const char *root, const char *abs, char *rel, size_t len);

/* Значение download_dir из config.txt → полный путь: относительное — от корня, полное — как есть,
 * пусто — «Downloads». 0 — готово. */
int dirpath_config(const char *root, const char *value, char *out, size_t len);

/* Папку с таким именем не показывать (в корне: система, программы, шрифты… и скрытые «.x»). */
int dirpath_hidden(const char *name, int at_root);

/* Подпапки rel в алфавитном порядке (скрытые, папка hide и ссылки наружу пропущены).
 * 0 — готово (*names — массив, освободить dirpath_free), -1 — папки нет или не открылась. */
int dirpath_subdirs(const char *root, const char *hide, const char *rel, char ***names, int *n);
void dirpath_free(char **names, int n);

/* Выбор «своей папки» у приёма и каталогов: файл с одной строкой — путь от корня; нет файла — общая.
 * load: 1 — своя папка (rel; «.» — сам корень), 0 — общая. save: rel пустой/NULL — вернуть общую
 * (файл удаляется); «.» — сам корень. */
int dirpath_choice_load(const char *file, char *rel, size_t len);
int dirpath_choice_save(const char *file, const char *rel);

#endif
