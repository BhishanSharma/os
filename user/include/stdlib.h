#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#define RAND_MAX 0x7FFFFFFF

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *memalign(size_t align, size_t size);              /* align: a power of two */
void *aligned_alloc(size_t align, size_t size);
int posix_memalign(void **out, size_t align, size_t size);

int atoi(const char *s);
long atol(const char *s);
int abs(int x);
int rand(void);
void srand(unsigned int seed);

void exit(int code) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
long long atoll(const char *s);
double strtod(const char *s, char **end);   /* these three are SSE code (libm.c) */
float strtof(const char *s, char **end);
double atof(const char *s);
long labs(long x);
long long llabs(long long x);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));
char *getenv(const char *name);             /* always 0: there is no environment */

#endif
