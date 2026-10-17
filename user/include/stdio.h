#ifndef STDIO_H
#define STDIO_H

#include <stdarg.h>
#include <stddef.h>

#define EOF (-1)

/* Formats: %d %i %u %x %X %o %p %c %s %f %e %g %%; flags - + space # 0;
 * width and precision (also *); sizes hh h l ll z j t. */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list args);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);
int sprintf(char *buf, const char *fmt, ...);

int putchar(int c);
int puts(const char *s);
int getchar(void);                         /* from the keyboard line; EOF never */
char *readline(char *buf, size_t size);    /* one line without the '\n' */

/* Streams, for ported code: stdout and stderr both go to the console. */
typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;
int fprintf(FILE *f, const char *fmt, ...);
int vfprintf(FILE *f, const char *fmt, va_list args);
int fputs(const char *s, FILE *f);
int fputc(int c, FILE *f);
int fflush(FILE *f);
size_t fwrite(const void *p, size_t size, size_t n, FILE *f);
int sscanf(const char *s, const char *fmt, ...);
FILE *fdopen(int fd, const char *mode);     /* always 0 */
int fclose(FILE *f);
FILE *fopen(const char *path, const char *mode);   /* always 0: use open() (os.h) */
size_t fread(void *p, size_t size, size_t n, FILE *f);
int setvbuf(FILE *f, char *buf, int mode, size_t size);
int feof(FILE *f);
int ferror(FILE *f);
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
#define BUFSIZ 1024
int vsscanf(const char *s, const char *fmt, va_list args);
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#endif
