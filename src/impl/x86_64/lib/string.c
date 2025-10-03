#include <stddef.h> // for size_t

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
