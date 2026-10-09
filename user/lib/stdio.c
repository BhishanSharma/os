// stdio.c - printf family and keyboard input
#include <stdio.h>
#include <string.h>
#include <os.h>

typedef void (*put_fn)(char c, void *ctx);

static void put_number(put_fn put, void *ctx, unsigned long value, int negative,
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

static void format(put_fn put, void *ctx, const char *fmt, va_list args) {
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
        while (*fmt == 'l') { is_long = 1; fmt++; }
        switch (*fmt) {
            case 'd':
            case 'i': {
                long v = is_long ? va_arg(args, long) : va_arg(args, int);
                put_number(put, ctx, v < 0 ? 0UL - (unsigned long)v : (unsigned long)v, v < 0, 10, 0, width, pad);
                break;
            }
            case 'u': {
                unsigned long v = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                put_number(put, ctx, v, 0, 10, 0, width, pad);
                break;
            }
            case 'x':
            case 'X': {
                unsigned long v = is_long ? va_arg(args, unsigned long) : va_arg(args, unsigned int);
                put_number(put, ctx, v, 0, 16, *fmt == 'X', width, pad);
                break;
            }
            case 'p':
                put('0', ctx);
                put('x', ctx);
                put_number(put, ctx, (unsigned long)va_arg(args, void *), 0, 16, 0, 0, ' ');
                break;
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
