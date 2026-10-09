#ifndef STDLIB_H
#define STDLIB_H

#include <stddef.h>

#define RAND_MAX 0x7FFFFFFF

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

int atoi(const char *s);
long atol(const char *s);
int abs(int x);
int rand(void);
void srand(unsigned int seed);

void exit(int code) __attribute__((noreturn));

#endif
