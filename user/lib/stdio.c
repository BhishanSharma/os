// stdio.c - printf family and keyboard input
#include <stdio.h>
#include <string.h>
#include <os.h>

typedef void (*put_fn)(char c, void *ctx);

/* One converted field: sign/prefix, then the body, padded to `width`. */
static void emit(put_fn put, void *ctx, const char *prefix, const char *body, int len,
                 int zeros, int width, int left, int zero_pad) {
    int plen = (int)strlen(prefix), total = plen + zeros + len;
    if (!left && !zero_pad) for (int i = total; i < width; i++) put(' ', ctx);
    while (*prefix) put(*prefix++, ctx);
    if (!left && zero_pad) for (int i = total; i < width; i++) put('0', ctx);
    for (int i = 0; i < zeros; i++) put('0', ctx);
    for (int i = 0; i < len; i++) put(body[i], ctx);
    if (left) for (int i = total; i < width; i++) put(' ', ctx);
}

/* Digits of v (no sign) into the end of buf -> where they start. */
static char *digits_of(char *end, unsigned long long v, unsigned base, int upper) {
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { *--end = d[v % base]; v /= base; } while (v);
    return end;
}

/* %f %e %g: fixed point to `prec` decimals (to 18 significant digits). */
static int fmt_float(char *out, double v, int prec, char conv, int alt) {
    int n = 0, upper = conv == 'E' || conv == 'G' || conv == 'F';
    if (v != v) { memcpy(out, upper ? "NAN" : "nan", 3); return 3; }
    if (v > 1.7976931348623157e308) { memcpy(out, upper ? "INF" : "inf", 3); return 3; }
    int exp10 = 0;
    char c = conv | 32;
    if (c == 'g') {
        if (prec == 0) prec = 1;
        double a = v;
        while (a >= 10) a /= 10, exp10++;
        while (a > 0 && a < 1) a *= 10, exp10--;
        if (exp10 < -4 || exp10 >= prec) { c = 'e'; prec--; }
        else { c = 'f'; prec = prec - 1 - exp10; }
        exp10 = 0;
    }
    if (c == 'e' && v != 0) {
        while (v >= 10) v /= 10, exp10++;
        while (v < 1) v *= 10, exp10--;
    }
    if (prec > 17) prec = 17;
    double r = 0.5;
    for (int i = 0; i < prec; i++) r /= 10;
    v += r;                                            /* round */
    if (c == 'e' && v >= 10) v /= 10, exp10++;
    if (v >= 1e19) {                                   /* too big for the integer part: e-notation */
        while (v >= 10) v /= 10, exp10++;
        c = 'e';
    }
    unsigned long long whole = (unsigned long long)v;
    double frac = v - (double)whole;
    char tmp[24], *w = digits_of(tmp + sizeof(tmp), whole, 10, 0);
    while (w < tmp + sizeof(tmp)) out[n++] = *w++;
    if (prec || alt) out[n++] = '.';
    for (int i = 0; i < prec; i++) {
        frac *= 10;
        int d = (int)frac;
        if (d > 9) d = 9;
        out[n++] = (char)('0' + d);
        frac -= d;
    }
    if ((conv | 32) == 'g' && !alt && prec) {          /* %g drops trailing zeros */
        while (out[n - 1] == '0') n--;
        if (out[n - 1] == '.') n--;
    }
    if (c == 'e') {
        out[n++] = upper ? 'E' : 'e';
        out[n++] = exp10 < 0 ? '-' : '+';
        int e = exp10 < 0 ? -exp10 : exp10;
        if (e >= 100) out[n++] = (char)('0' + e / 100);
        out[n++] = (char)('0' + e / 10 % 10);
        out[n++] = (char)('0' + e % 10);
    }
    return n;
}

static void format(put_fn put, void *ctx, const char *fmt, va_list args) {
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(*fmt, ctx);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, width = 0, prec = -1, size = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        if (*fmt == '*') {
            width = va_arg(args, int);
            if (width < 0) left = 1, width = -width;
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = va_arg(args, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt++ - '0');
        }
        for (;; fmt++) {                               /* size: <0 hh/h, 1 l, 2 ll, z, j, t */
            if (*fmt == 'h') size--;
            else if (*fmt == 'l') size++;
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') size = 2;
            else if (*fmt == 'L') size = 3;
            else break;
        }
        char buf[352], *body;
        int len;
        const char *prefix = "";
        switch (*fmt) {
            case 'd':
            case 'i': {
                long long v = size >= 1 ? va_arg(args, long long) : va_arg(args, int);
                if (size == -1) v = (short)v;
                if (size <= -2) v = (signed char)v;
                unsigned long long m = v < 0 ? 0ULL - (unsigned long long)v : (unsigned long long)v;
                prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
                body = digits_of(buf + sizeof(buf), m, 10, 0);
                len = (int)(buf + sizeof(buf) - body);
                if (prec == 0 && m == 0) len = 0;
                emit(put, ctx, prefix, body, len, prec > len ? prec - len : 0, width, left, zero && prec < 0);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o': {
                unsigned long long v = size >= 1 ? va_arg(args, unsigned long long) : va_arg(args, unsigned int);
                if (size == -1) v = (unsigned short)v;
                if (size <= -2) v = (unsigned char)v;
                unsigned base = *fmt == 'u' ? 10 : *fmt == 'o' ? 8 : 16;
                body = digits_of(buf + sizeof(buf), v, base, *fmt == 'X');
                len = (int)(buf + sizeof(buf) - body);
                if (alt && v && base == 16) prefix = *fmt == 'X' ? "0X" : "0x";
                if (alt && base == 8) prefix = "0";
                if (prec == 0 && v == 0) len = 0;
                emit(put, ctx, prefix, body, len, prec > len ? prec - len : 0, width, left, zero && prec < 0);
                break;
            }
            case 'p':
                body = digits_of(buf + sizeof(buf), (unsigned long)va_arg(args, void *), 16, 0);
                emit(put, ctx, "0x", body, (int)(buf + sizeof(buf) - body), 0, width, left, 0);
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G': {
                double v = size == 3 ? (double)va_arg(args, long double) : va_arg(args, double);
                prefix = v < 0 ? "-" : plus ? "+" : space ? " " : "";
                if (v < 0) v = -v;
                len = fmt_float(buf, v, prec < 0 ? 6 : prec, *fmt, alt);
                emit(put, ctx, prefix, buf, len, 0, width, left, zero);
                break;
            }
            case 's': {
                const char *s = va_arg(args, const char *);
                if (!s) s = "(null)";
                len = 0;
                while ((prec < 0 || len < prec) && s[len]) len++;
                emit(put, ctx, "", s, len, 0, width, left, 0);
                break;
            }
            case 'c':
                buf[0] = (char)va_arg(args, int);
                emit(put, ctx, "", buf, 1, 0, width, left, 0);
                break;
            case 'n':
                va_arg(args, int *);                   /* not supported: skipped */
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

/* printf collects output here and writes it with one system call per 256 bytes. */
struct out_buf {
    char data[256];
    size_t len, total;
};

static void out_put(char c, void *p) {
    struct out_buf *o = p;
    o->data[o->len++] = c;
    o->total++;
    if (o->len == sizeof(o->data)) {
        write(1, o->data, o->len);
        o->len = 0;
    }
}

int vprintf(const char *fmt, va_list args) {
    struct out_buf o;
    o.len = o.total = 0;
    format(out_put, &o, fmt, args);
    if (o.len) write(1, o.data, o.len);
    return (int)o.total;
}

int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vprintf(fmt, args);
    va_end(args);
    return n;
}

struct str_buf {
    char *buf;
    size_t size, pos;
};

static void str_put(char c, void *p) {
    struct str_buf *s = p;
    if (s->pos + 1 < s->size) s->buf[s->pos] = c;
    s->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list args) {
    struct str_buf s = { buf, size, 0 };
    format(str_put, &s, fmt, args);
    if (size) buf[s.pos < size ? s.pos : size - 1] = 0;
    return (int)s.pos;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, (size_t)-1 / 2, fmt, args);
    va_end(args);
    return n;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    write(1, s, strlen(s));
    write(1, "\n", 1);
    return 0;
}

int getchar(void) {
    char c;
    return read(0, &c, 1) == 1 ? (unsigned char)c : EOF;
}

char *readline(char *buf, size_t size) {
    size_t n = 0;
    while (1) {
        int c = getchar();
        if (c == EOF || c == '\n') break;
        if (n + 1 < size) buf[n++] = (char)c;
    }
    buf[n] = 0;
    return buf;
}
