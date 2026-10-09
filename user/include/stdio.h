#ifndef STDIO_H
#define STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

/* Formats: %d %i %u %x %X %p %c %s %%, the l modifier, widths (%5d, %-10s)
 * and zero padding (%08x). No floating point. */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int sprintf(char *buf, const char *fmt, ...);

int putchar(int c);
int puts(const char *s);
int getchar(void);                         /* from the keyboard line; EOF never */
char *readline(char *buf, size_t size);    /* one line without the '\n' */

#endif
