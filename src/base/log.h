/* Лог с метками времени в миллисекундах от log_open. */
#ifndef WASHI_BASE_LOG_H
#define WASHI_BASE_LOG_H

#include <stdio.h>

/* Открывает лог на дозапись (больше 512 КБ — прежний переименовывается в <path>.1).
 * Без log_open сообщения идут в stderr. */
int log_open(const char *path);
void log_close(void);

void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Сбросить лог на диск (fsync): перед выходом, уходом в фон. */
void log_sync(void);

#endif
