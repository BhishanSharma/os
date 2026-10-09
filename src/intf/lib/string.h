#ifndef STRING_H
#define STRING_H
#include <stddef.h>
#include <stdarg.h>
int strcmp(const char* s1, const char* s2);
int strncmp(const char* s1, const char* s2, size_t n);
size_t strlen(const char* str);
/* printf-style formatting shared by kprintf and k_snprintf: %d %i %u %x %X %c
 * %s %b %%, the l modifier for 64-bit values (%lu %lx), a width (%5u, %-12s)
 * and zero padding (%02X). %x has no "0x" prefix, as in printf. */
typedef void (*k_putc_fn)(char c, void *ctx);
void k_vformat(k_putc_fn put, void *ctx, const char *fmt, va_list args);
int k_vsnprintf(char *buffer, size_t size, const char *fmt, va_list args);
int k_snprintf(char* buffer, size_t size, const char* fmt, ...);
void kstrncpy(char* dest, const char* src, size_t n);
char* kstrtok(char* str, const char* delim, char** saveptr);
int kstr_contains(const char *haystack, const char *needle);

// Memory helpers. memset lives in drivers/memory.c, the others in lib/string.c.
// (GCC may also emit calls to these on its own, so they must always exist.)
void* memset(void* ptr, int value, size_t num);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
int memcmp(const void* a, const void* b, size_t n);

#endif
