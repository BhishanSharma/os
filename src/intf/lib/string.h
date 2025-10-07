#ifndef STRING_H
#define STRING_H
#include <stddef.h>
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
size_t strlen(const char* str);
int k_snprintf(char* buffer, size_t size, const char* fmt, ...);
void kstrncpy(char* dest, const char* src, size_t n);
char* kstrtok(char* str, const char* delim, char** saveptr);
int kstr_contains(const char *haystack, const char *needle);

#endif
