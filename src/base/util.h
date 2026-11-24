/* util — мелочи, нужные всем слоям: запись файла с заменой, обрезка строки. */
#ifndef WASHI_BASE_UTIL_H
#define WASHI_BASE_UTIL_H

#include <stdio.h>

/* Запись файла целиком так, чтобы при сбое остался прежний: пишем в «path.tmp»,
 * file_save_end переименовывает его в path. sync — fsync перед заменой (данные пользователя;
 * для кэша не нужен: на FAT книги он дорог). f == NULL допустим. 0 — файл заменён. */
FILE *file_save_begin(const char *path);
int file_save_end(FILE *f, const char *path, int sync);

/* Убрать пробелы, табуляции и перевод строки по краям (на месте); возвращает начало. */
char *str_trim(char *s);

#endif
