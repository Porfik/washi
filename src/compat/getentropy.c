/* getentropy() для glibc 2.23 на книге: функция появилась только в glibc 2.25,
 * а MuPDF (source/fitz/random.c) её вызывает. Собирается только под PB740. */
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>

int getentropy(void *buf, size_t len);

int getentropy(void *buf, size_t len)
{
    if (len > 256) {
        errno = EIO;
        return -1;
    }
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    unsigned char *p = buf;
    while (len > 0) {
        ssize_t n = read(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            close(fd);
            errno = EIO;
            return -1;
        }
        p += n;
        len -= (size_t)n;
    }
    close(fd);
    return 0;
}
