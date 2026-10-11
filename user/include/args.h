/* args.h - command-line helpers for programs */
#ifndef ARGS_H
#define ARGS_H

#include <stddef.h>
#include <string.h>

/* argv[from], argv[from + 1], ... joined with spaces into `out`: a file name
 * with spaces works without quotes ("cat my notes.txt"). Returns out. */
static inline char *args_join(char *out, size_t size, int argc, char **argv, int from) {
    size_t n = 0;
    out[0] = 0;
    for (int i = from; i < argc && n + 1 < size; i++) {
        if (i > from) out[n++] = ' ';
        for (const char *s = argv[i]; *s && n + 1 < size; s++) out[n++] = *s;
    }
    out[n] = 0;
    return out;
}

#endif
