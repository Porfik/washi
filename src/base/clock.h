#ifndef WASHI_BASE_CLOCK_H
#define WASHI_BASE_CLOCK_H

#include <time.h>

/* Монотонное время в миллисекундах. */
static inline double clock_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

#endif
