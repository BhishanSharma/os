#include <stddef.h> // for size_t
#include <stdarg.h>

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

int k_snprintf(char* buffer, size_t size, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);

    size_t pos = 0;

    for (const char* p = fmt; *p && pos < size - 1; p++) {
        if (*p != '%') {
            buffer[pos++] = *p;
            continue;
        }

        p++; // skip '%'

        if (*p == 'c') {
            char c = (char)va_arg(args, int);
            if (pos < size - 1) buffer[pos++] = c;
        } else if (*p == 's') {
            const char* s = va_arg(args, const char*);
            while (*s && pos < size - 1) buffer[pos++] = *s++;
        } else if (*p == 'd' || *p == 'i') {
            int val = va_arg(args, int);
            char temp[12];
            int neg = 0;
            int tpos = 0;

            if (val < 0) {
                neg = 1;
                val = -val;
            }

            // Convert number to string
            do {
                temp[tpos++] = '0' + (val % 10);
                val /= 10;
            } while (val && tpos < sizeof(temp));

            if (neg && tpos < sizeof(temp)) temp[tpos++] = '-';

            // Reverse string
            for (int i = tpos - 1; i >= 0 && pos < size - 1; i--) {
                buffer[pos++] = temp[i];
            }
        } else if (*p == 'x' || *p == 'X') {
            unsigned int val = va_arg(args, unsigned int);
            char temp[9];
            int tpos = 0;
            const char* hex = (*p == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";

            do {
                temp[tpos++] = hex[val & 0xF];
                val >>= 4;
            } while (val && tpos < sizeof(temp));

            // Reverse string
            for (int i = tpos - 1; i >= 0 && pos < size - 1; i--) {
                buffer[pos++] = temp[i];
            }
        } else if (*p == '%') {
            buffer[pos++] = '%';
        }
    }

    buffer[pos] = '\0';
    va_end(args);
    return (int)pos;
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
