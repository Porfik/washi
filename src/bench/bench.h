#ifndef WASHI_BENCH_H
#define WASHI_BENCH_H

#include <stdio.h>

/* washi --bench FILE [PAGES] [опции]; argv[0] — первый аргумент после --bench.
 * Отчёт пишется в out. Возвращает код выхода. */
int bench_main(int argc, char **argv, FILE *out);

void bench_usage(FILE *out);

#endif
