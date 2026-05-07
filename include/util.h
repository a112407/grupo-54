#ifndef UTIL_H
#define UTIL_H

#include <unistd.h>

/* Escreve uma string para um descritor de ficheiro */
void write_str(int fd, const char *s);

/* Garante escrita total, tratando escritas parciais e EINTR */
ssize_t write_all(int fd, const void *buf, size_t len);

/* Garante leitura total, tratando leituras parciais e EINTR */
ssize_t read_all(int fd, void *buf, size_t len);

#endif /* UTIL_H */
