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

// Memory helpers. memset lives in drivers/memory.c, the others in lib/string.c.
// (GCC may also emit calls to these on its own, so they must always exist.)
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);

#endif
