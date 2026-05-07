#include <string.h>
#include <errno.h>
#include <unistd.h>
#include "../include/util.h"

void write_str(int fd, const char *s) {
    write(fd, s, strlen(s));
}

/* Garante escrita total, tratando escritas parciais e EINTR */
ssize_t write_all(int fd, const void *buf, size_t len) {
    size_t total = 0;
    const char *ptr = (const char *)buf;

    while (total < len) {
        ssize_t n = write(fd, ptr + total, len - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}

/* Garante leitura total, tratando leituras parciais e EINTR */
ssize_t read_all(int fd, void *buf, size_t len) {
    size_t total = 0;
    char *ptr = (char *)buf;

    while (total < len) {
        ssize_t n = read(fd, ptr + total, len - total);
        if (n == 0) return (total == 0) ? 0 : -1;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        total += (size_t)n;
    }
    return (ssize_t)total;
}
