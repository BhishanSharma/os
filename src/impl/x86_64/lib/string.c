#include <stddef.h> // for size_t
#include <stdint.h>
#include <stdarg.h>
#include "lib/string.h"

int strcmp(const char* s1, const char* s2) {
    while (*s1 && *s2) {
        if (*s1 != *s2)
            return (unsigned char)*s1 - (unsigned char)*s2;
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

int strncmp(const char* s1, const char* s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s1[i] != s2[i])
            return (unsigned char)s1[i] - (unsigned char)s2[i];
        if (s1[i] == '\0')  // reached end of string
            return 0;
    }
    return 0;
}

size_t strlen(const char* str) {
    size_t len = 0;
    while (str[len]) {
        len++;
    }
    return len;
}

/* One number: `base` 10 or 16, padded to `width` with `pad` ('0' or ' '); a
 * minus sign goes before zero padding, as in printf. */
static void fmt_number(k_putc_fn put, void *ctx, uint64_t value, int negative,
                       unsigned base, int upper, int width, char pad) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int n = 0;
    do {
        tmp[n++] = digits[value % base];
        value /= base;
    } while (value);
    int len = n + negative;
    if (negative && pad == '0') put('-', ctx);
    for (; len < width; len++) put(pad, ctx);
    if (negative && pad != '0') put('-', ctx);
    while (n) put(tmp[--n], ctx);
}

void k_vformat(k_putc_fn put, void *ctx, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(*fmt, ctx);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, left = 0, is_long = 0;
        if (*fmt == '-') { left = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == 'l') { is_long = 1; fmt++; }
        switch (*fmt) {
            case 'd':
            case 'i': {
                int64_t val = is_long ? va_arg(args, int64_t) : va_arg(args, int);
                uint64_t mag = val < 0 ? (uint64_t)0 - (uint64_t)val : (uint64_t)val;
                fmt_number(put, ctx, mag, val < 0, 10, 0, width, pad);
                break;
            }
            case 'u': {
                uint64_t val = is_long ? va_arg(args, uint64_t) : va_arg(args, uint32_t);
                fmt_number(put, ctx, val, 0, 10, 0, width, pad);
                break;
            }
            case 'x':
            case 'X': {
                uint64_t val = is_long ? va_arg(args, uint64_t) : va_arg(args, uint32_t);
                fmt_number(put, ctx, val, 0, 16, *fmt == 'X', width, pad);
                break;
            }
            case 'b': {
                uint32_t val = va_arg(args, uint32_t);
                put('0', ctx); put('b', ctx);
                for (int i = 31; i >= 0; i--) {
                    put((val & (1u << i)) ? '1' : '0', ctx);
                    if (i % 8 == 0 && i != 0) put('_', ctx);
                }
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                int len = (int)strlen(s);
                if (!left) for (int i = len; i < width; i++) put(' ', ctx);
                while (*s) put(*s++, ctx);
                if (left) for (int i = len; i < width; i++) put(' ', ctx);
                break;
            }
            case 'c':
                put((char)va_arg(args, int), ctx);
                break;
            case '%':
                put('%', ctx);
                break;
            case 0:
                return;
            default:
                put('%', ctx);
                put(*fmt, ctx);
                break;
        }
    }
}

struct snprintf_ctx {
    char *buffer;
    size_t size, pos;
};

static void snprintf_put(char c, void *p) {
    struct snprintf_ctx *ctx = p;
    if (ctx->pos + 1 < ctx->size) ctx->buffer[ctx->pos] = c;
    ctx->pos++;
}

int k_vsnprintf(char *buffer, size_t size, const char *fmt, va_list args) {
    struct snprintf_ctx ctx = { buffer, size, 0 };
    k_vformat(snprintf_put, &ctx, fmt, args);
    if (size) buffer[ctx.pos < size ? ctx.pos : size - 1] = 0;
    return (int)ctx.pos;
}

int k_snprintf(char *buffer, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = k_vsnprintf(buffer, size, fmt, args);
    va_end(args);
    return n;
}

// strncpy replacement
void kstrncpy(char* dest, const char* src, size_t n) {
    size_t i;
    for (i = 0; i < n-1 && src[i]; i++) {
        dest[i] = src[i];
    }
    dest[i] = '\0';
}

// strchr replacement for kernel
char* kstrchr(const char* s, char c) {
    while (*s) {
        if (*s == c) return (char*)s;
        s++;
    }
    return NULL;
}

// simple path splitter
char* kstrtok(char* str, const char* delim, char** saveptr) {
    char* token;
    if (str) *saveptr = str;
    if (!*saveptr) return NULL;

    token = *saveptr;
    while (**saveptr && !kstrchr(delim, **saveptr)) (*saveptr)++;
    if (**saveptr) {
        **saveptr = '\0';
        (*saveptr)++;
    } else {
        *saveptr = NULL;
    }
    return token;
}

int kstr_contains(const char *haystack, const char *needle) {
    if (!*needle) return 1; // empty needle always "found"

    for (; *haystack; haystack++) {
        const char *h = haystack;
        const char *n = needle;

        while (*h && *n && (*h == *n)) {
            h++;
            n++;
        }

        if (*n == '\0') {
            return 1; // found substring
        }
    }

    return 0; // not found
}

// The attribute stops GCC from "optimising" these loops into a call to
// memcpy/memcmp itself, which would be infinite recursion.
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    while (n--) {
        *d++ = *s++;
    }
    return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if ((uintptr_t)d <= (uintptr_t)s) {
        for (size_t i = 0; i < n; ++i) d[i] = s[i];
    } else {
        while (n) {
            --n;
            d[n] = s[n];
        }
    }
    return dest;
}

__attribute__((optimize("no-tree-loop-distribute-patterns")))
int memcmp(const void* a, const void* b, size_t n) {
    const unsigned char* pa = (const unsigned char*)a;
    const unsigned char* pb = (const unsigned char*)b;
    for (size_t i = 0; i < n; i++) {
        if (pa[i] != pb[i])
            return (int)pa[i] - (int)pb[i];
    }
    return 0;
}
