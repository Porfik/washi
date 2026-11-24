#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "clock.h"

#define LOG_MAX_BYTES (512 * 1024)

static FILE *log_file;
static double log_start_ms;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int log_open(const char *path)
{
    log_start_ms = clock_ms();
    /* Лог дописывается при каждом запуске: вырос больше LOG_MAX_BYTES — старый становится
     * <path>.1 (прошлый .1 пропадает), новый начинается с нуля. */
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > LOG_MAX_BYTES) {
        char old[1100];
        snprintf(old, sizeof old, "%s.1", path);
        rename(path, old);
    }
    log_file = fopen(path, "a");
    if (!log_file)
        return -1;
    time_t now = time(NULL);
    char date[64];
    strftime(date, sizeof date, "%Y-%m-%d %H:%M:%S", localtime(&now));
    fprintf(log_file, "\n==== %s\n", date);
    fflush(log_file);
    return 0;
}

void log_close(void)
{
    if (!log_file)
        return;
    log_sync();
    fclose(log_file);
    log_file = NULL;
}

void log_msg(const char *fmt, ...)
{
    FILE *f = log_file ? log_file : stderr;
    va_list ap;
    pthread_mutex_lock(&log_lock);
    fprintf(f, "[%8.1f] ", clock_ms() - log_start_ms);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    /* fflush, но не fsync: данные в кэше ядра переживут падение приложения,
     * а система сбрасывает их сама перед подключением книги к ПК. */
    fflush(f);
    pthread_mutex_unlock(&log_lock);
}

void log_sync(void)
{
    if (!log_file)
        return;
    pthread_mutex_lock(&log_lock);
    fflush(log_file);
    fsync(fileno(log_file));
    pthread_mutex_unlock(&log_lock);
}
