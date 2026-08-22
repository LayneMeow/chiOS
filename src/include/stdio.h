#ifndef _STDIO_H
#define _STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

int putchar(int c);
int puts(const char *s);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vprintf(const char *fmt, va_list ap);

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

int sprintf(char *buf, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vsprintf(char *buf, const char *fmt, va_list ap);

int getchar(void);

char *gets_s(char *s, size_t size);

int scanf(const char *fmt, ...) __attribute__((format(scanf, 1, 2)));
int vscanf(const char *fmt, va_list ap);

int sscanf(const char *str, const char *fmt, ...)
    __attribute__((format(scanf, 2, 3)));
int vsscanf(const char *str, const char *fmt, va_list ap);

#endif
