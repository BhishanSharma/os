// libc2.c - more of the C library, for programs ported from elsewhere
// (FFmpeg): number parsing, sorting, more string functions, errno, assert,
// the FILE streams (stdout and stderr only: both go to the console), time.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>
#include <os.h>
#include <sys/stat.h>

int errno;

void abort(void) {
    printf("abort()\n");
    exit(134);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func) {
    printf("Assertion failed: %s (%s: %s: %d)\n", expr, file, func, line);
    abort();
}

/* ---- Numbers ------------------------------------------------------------------- */

static int digit(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
}

/* -> the magnitude; *neg the sign; *end where it stopped (s if nothing). */
static unsigned long long parse(const char *s, char **end, int base, int *neg, int *overflow) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    *neg = 0;
    *overflow = 0;
    if (*p == '+' || *p == '-') *neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x' && digit(p[2]) < 16) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = p[0] == '0' ? 8 : 10;
    }
    unsigned long long v = 0;
    const char *first = p;
    for (int d; (d = digit(*p)) < base; p++) {
        if (v > (~0ULL - d) / base) *overflow = 1;
        v = v * base + d;
    }
    if (end) *end = (char *)(p == first ? s : p);
    return v;
}

long long strtoll(const char *s, char **end, int base) {
    int neg, of;
    unsigned long long v = parse(s, end, base, &neg, &of);
    if (of || v > (neg ? 1ULL << 63 : (1ULL << 63) - 1)) {
        errno = ERANGE;
        return neg ? (long long)(1ULL << 63) : (long long)((1ULL << 63) - 1);
    }
    return neg ? (long long)(0 - v) : (long long)v;
}
unsigned long long strtoull(const char *s, char **end, int base) {
    int neg, of;
    unsigned long long v = parse(s, end, base, &neg, &of);
    if (of) { errno = ERANGE; return ~0ULL; }
    return neg ? 0 - v : v;
}
long strtol(const char *s, char **end, int base) { return strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return strtoull(s, end, base); }
intmax_t strtoimax(const char *s, char **end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoull(s, end, base); }
long long atoll(const char *s) { return strtoll(s, 0, 10); }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }

char *getenv(const char *name) { (void)name; return 0; }

/* ---- Sorting and searching ------------------------------------------------------ */

static void swap(char *a, char *b, size_t n) {
    while (n--) { char t = *a; *a++ = *b; *b++ = t; }
}

/* Heap sort: no recursion, no extra memory, n log n. */
static void sift(char *base, size_t root, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    for (size_t child; (child = 2 * root + 1) < n; root = child) {
        if (child + 1 < n && cmp(base + child * size, base + (child + 1) * size) < 0) child++;
        if (cmp(base + root * size, base + child * size) >= 0) return;
        swap(base + root * size, base + child * size, size);
    }
}
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    char *b = base;
    for (size_t i = n / 2; i-- > 0;) sift(b, i, n, size, cmp);
    for (size_t end = n; end-- > 1;) {
        swap(b, b + end * size, size);
        sift(b, 0, end, size, cmp);
    }
}

void *bsearch(const void *key, const void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    const char *b = base;
    while (n) {
        const char *mid = b + n / 2 * size;
        int c = cmp(key, mid);
        if (!c) return (void *)mid;
        if (c > 0) { b = mid + size; n -= n / 2 + 1; }
        else n /= 2;
    }
    return 0;
}

/* ---- Strings -------------------------------------------------------------------- */

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;
    for (; n--; p++)
        if (*p == (unsigned char)c) return (void *)p;
    return 0;
}
size_t strnlen(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}
size_t strspn(const char *s, const char *accept) {
    size_t n = 0;
    while (s[n] && strchr(accept, s[n])) n++;
    return n;
}
size_t strcspn(const char *s, const char *reject) {
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n])) n++;
    return n;
}
char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : 0;
}
char *strncat(char *dst, const char *src, size_t n) {
    char *d = dst + strlen(dst);
    while (n-- && *src) *d++ = *src++;
    *d = 0;
    return dst;
}
char *strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    return d ? memcpy(d, s, n) : 0;
}
char *strndup(const char *s, size_t max) {
    size_t n = strnlen(s, max);
    char *d = malloc(n + 1);
    if (!d) return 0;
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int strcasecmp(const char *a, const char *b) {
    while (*a && lower(*a) == lower(*b)) a++, b++;
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}
int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d || !*a) return d;
    }
    return 0;
}
char *strtok_r(char *s, const char *delim, char **save) {
    if (!s) s = *save;
    s += strspn(s, delim);
    if (!*s) { *save = s; return 0; }
    char *end = s + strcspn(s, delim);
    if (*end) *end++ = 0;
    *save = end;
    return s;
}
char *strerror(int e) {
    switch (e) {
    case 0: return "no error";
    case ENOENT: return "no such file";
    case ENOMEM: return "out of memory";
    case EINVAL: return "invalid argument";
    case EIO: return "input/output error";
    case EAGAIN: return "try again";
    case ENOSYS: return "not implemented";
    case ERANGE: return "out of range";
    case EPERM: case EACCES: return "not allowed";
    default: return "error";
    }
}
int strerror_r(int e, char *buf, size_t n) {
    snprintf(buf, n, "%s", strerror(e));
    return 0;
}

/* ---- Streams: stdout and stderr, both the console --------------------------------- */

struct FILE { int fd; };
static FILE out_stream = { 1 }, err_stream = { 2 };
FILE *stdout = &out_stream, *stderr = &err_stream, *stdin = 0;

int vfprintf(FILE *f, const char *fmt, va_list args) { (void)f; return vprintf(fmt, args); }
int fprintf(FILE *f, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(f, fmt, args);
    va_end(args);
    return n;
}
int fputs(const char *s, FILE *f) { return fprintf(f, "%s", s); }
int fputc(int c, FILE *f) { (void)f; return putchar(c); }
int fflush(FILE *f) { (void)f; return 0; }
size_t fwrite(const void *p, size_t size, size_t n, FILE *f) {
    const char *c = p;
    for (size_t i = 0; i < size * n; i++) fputc(c[i], f);
    return n;
}

/* ---- Files: POSIX names over the system's ---------------------------------------- */

int fstat(int fd, struct stat *st) {
    long here = lseek(fd, 0, SEEK_CUR), size = lseek(fd, 0, SEEK_END);
    lseek(fd, here, SEEK_SET);
    if (size < 0) { errno = EBADF; return -1; }
    memset(st, 0, sizeof(*st));
    st->st_size = size;
    st->st_mode = S_IFREG;
    return 0;
}

/* ---- Time ------------------------------------------------------------------------- */

clock_t clock(void) { return (clock_t)uptime_ms(); }
int clock_gettime(int clk, struct timespec *ts) {
    unsigned long ms = uptime_ms();
    (void)clk;
    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)(ms % 1000) * 1000000;
    return 0;
}
time_t time(time_t *t) {
    struct os_time now;
    struct tm tm = { 0 };
    if (gettime_utc(&now) == 0) {
        tm.tm_year = now.year - 1900;
        tm.tm_mon = now.month - 1;
        tm.tm_mday = now.day;
        tm.tm_hour = now.hour;
        tm.tm_min = now.minute;
        tm.tm_sec = now.second;
    }
    time_t v = mktime(&tm);
    if (t) *t = v;
    return v;
}
struct tm *gmtime_r(const time_t *t, struct tm *tm) {
    long days = *t / 86400, secs = *t % 86400;
    if (secs < 0) secs += 86400, days--;
    memset(tm, 0, sizeof(*tm));
    tm->tm_hour = (int)(secs / 3600);
    tm->tm_min = (int)(secs / 60 % 60);
    tm->tm_sec = (int)(secs % 60);
    tm->tm_wday = (int)((days + 4) % 7 + 7) % 7;
    /* days since 1970 -> civil date (Howard Hinnant's algorithm) */
    long z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    tm->tm_year = (int)(y - 1900);
    tm->tm_mon = (int)(m - 1);
    tm->tm_mday = (int)d;
    return tm;
}
struct tm *localtime_r(const time_t *t, struct tm *tm) { return gmtime_r(t, tm); }
struct tm *gmtime(const time_t *t) { static struct tm tm; return gmtime_r(t, &tm); }
struct tm *localtime(const time_t *t) { return gmtime(t); }
time_t mktime(struct tm *tm) {
    long y = tm->tm_year + 1900L, m = tm->tm_mon + 1;
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400;
    long doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + tm->tm_mday - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = era * 146097 + doe - 719468;
    return (time_t)(days * 86400 + tm->tm_hour * 3600L + tm->tm_min * 60L + tm->tm_sec);
}
size_t strftime(char *s, size_t max, const char *fmt, const struct tm *tm) {
    size_t n = 0;
    for (; *fmt && n + 1 < max; fmt++) {
        if (*fmt != '%') { s[n++] = *fmt; continue; }
        char tmp[16];
        switch (*++fmt) {
        case 'Y': snprintf(tmp, sizeof(tmp), "%d", tm->tm_year + 1900); break;
        case 'm': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1); break;
        case 'd': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday); break;
        case 'H': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour); break;
        case 'M': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min); break;
        case 'S': snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec); break;
        case 0: fmt--; tmp[0] = 0; break;
        default: tmp[0] = '%'; tmp[1] = *fmt; tmp[2] = 0; break;
        }
        for (char *t = tmp; *t && n + 1 < max; t++) s[n++] = *t;
    }
    if (max) s[n] = 0;
    return n;
}

/* ---- sscanf ------------------------------------------------------------------------ */

int vsscanf(const char *s, const char *fmt, va_list args) {
    const char *p = s;
    int count = 0;
    for (; *fmt; fmt++) {
        if (*fmt == ' ' || (*fmt >= '\t' && *fmt <= '\r')) {
            while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') fmt++;
            if (*p != *fmt) break;
            p++;
            continue;
        }
        fmt++;
        int skip = 0, width = 0, size = 0;            /* size: -2 hh, -1 h, 1 l, 2 ll */
        if (*fmt == '*') { skip = 1; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        for (;; fmt++) {
            if (*fmt == 'h') size--;
            else if (*fmt == 'l') size++;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') size = 2;
            else if (*fmt == 'L') size = 3;
            else break;
        }
        char c = *fmt;
        if (!c) break;
        if (c != 'c' && c != '[' && c != 'n')
            while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
        if (c == 'n') {
            if (!skip) *va_arg(args, int *) = (int)(p - s);
            continue;
        }
        if (!*p) return count ? count : EOF;
        char buf[64];
        const char *src = p;
        if (width && width < 63 && c != 's' && c != 'c' && c != '[') {   /* limit a number's length */
            memcpy(buf, p, width);
            buf[width] = 0;
            src = buf;
        }
        if (c == 'd' || c == 'i' || c == 'u' || c == 'x' || c == 'X' || c == 'o') {
            char *end;
            int base = c == 'd' || c == 'u' ? 10 : c == 'i' ? 0 : c == 'o' ? 8 : 16;
            unsigned long long v = (unsigned long long)strtoll(src, &end, base);
            if (c != 'd' && c != 'i') v = strtoull(src, &end, base);
            if (end == src) break;
            p += end - src;
            if (!skip) {
                void *dst = va_arg(args, void *);
                if (size <= -2) *(char *)dst = (char)v;
                else if (size == -1) *(short *)dst = (short)v;
                else if (size == 0) *(int *)dst = (int)v;
                else *(long long *)dst = (long long)v;
                count++;
            }
        } else if (c == 'f' || c == 'e' || c == 'g' || c == 'E' || c == 'G' || c == 'a') {
            char *end;
            double v = strtod(src, &end);
            if (end == src) break;
            p += end - src;
            if (!skip) {
                if (size >= 1) *va_arg(args, double *) = v;
                else *va_arg(args, float *) = (float)v;
                count++;
            }
        } else if (c == 's') {
            char *dst = skip ? 0 : va_arg(args, char *);
            int n = 0;
            while (*p && !(*p == ' ' || (*p >= '\t' && *p <= '\r')) && (!width || n < width)) {
                if (dst) dst[n] = *p;
                n++, p++;
            }
            if (dst) { dst[n] = 0; count++; }
        } else if (c == 'c') {
            char *dst = skip ? 0 : va_arg(args, char *);
            if (!width) width = 1;
            for (int n = 0; n < width; n++) {
                if (!*p) return count;
                if (dst) dst[n] = *p;
                p++;
            }
            if (dst) count++;
        } else if (c == '[') {
            int neg = *++fmt == '^';
            if (neg) fmt++;
            const char *set = fmt;
            if (*fmt == ']') fmt++;
            while (*fmt && *fmt != ']') fmt++;
            int setlen = (int)(fmt - set);
            char *dst = skip ? 0 : va_arg(args, char *);
            int n = 0;
            while (*p && (!width || n < width)) {
                int in = 0;
                for (int k = 0; k < setlen; k++) {
                    if (k + 2 < setlen && set[k + 1] == '-') {
                        if (*p >= set[k] && *p <= set[k + 2]) in = 1;
                        k += 2;
                    } else if (*p == set[k]) {
                        in = 1;
                    }
                }
                if (in == neg) break;
                if (dst) dst[n] = *p;
                n++, p++;
            }
            if (!n) break;
            if (dst) { dst[n] = 0; count++; }
        } else {
            break;
        }
    }
    return count;
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsscanf(s, fmt, args);
    va_end(args);
    return n;
}

/* ---- No FILE from a descriptor ---------------------------------------------------- */

FILE *fdopen(int fd, const char *mode) { (void)fd, (void)mode; errno = ENOSYS; return 0; }
int fclose(FILE *f) { (void)f; return 0; }

int usleep(unsigned int us) { sleep_ms((us + 999) / 1000); return 0; }
unsigned int sleep(unsigned int s) { sleep_ms(s * 1000UL); return 0; }
FILE *fopen(const char *path, const char *mode) { (void)path, (void)mode; errno = ENOENT; return 0; }
size_t fread(void *p, size_t size, size_t n, FILE *f) { (void)p, (void)size, (void)n, (void)f; return 0; }
int setvbuf(FILE *f, char *buf, int mode, size_t size) { (void)f, (void)buf, (void)mode, (void)size; return 0; }
int feof(FILE *f) { (void)f; return 1; }
int ferror(FILE *f) { (void)f; return 0; }
