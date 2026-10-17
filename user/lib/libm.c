// libm.c - math.h: floating point maths, and strtod.
//
// Built with SSE allowed (see the Makefile), for ports such as FFmpeg. The
// hard functions use the x87 FPU's own instructions (fsin, fyl2x, f2xm1,
// fpatan, fprem): short, and accurate to double precision for the values
// decoders use. The kernel saves every task's FPU and SSE registers.
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <errno.h>

typedef union { double d; uint64_t u; } dbits;

double fabs(double x) { dbits b = { x }; b.u &= ~(1ULL << 63); return b.d; }
float fabsf(float x) { return (float)fabs(x); }
double copysign(double x, double y) {
    dbits a = { x }, b = { y };
    a.u = (a.u & ~(1ULL << 63)) | (b.u & (1ULL << 63));
    return a.d;
}
float copysignf(float x, float y) { return (float)copysign(x, y); }

double sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
float sqrtf(float x) { float r; __asm__("sqrtss %1, %0" : "=x"(r) : "x"(x)); return r; }

/* Whole numbers: below 2^52 a double's fraction can be cut off by a cast. */
double trunc(double x) {
    if (!(fabs(x) < 4503599627370496.0)) return x;   /* big, inf or NaN: already whole */
    return copysign((double)(long long)x, x);
}
double floor(double x) { double t = trunc(x); return t > x ? t - 1 : t; }
double ceil(double x) { double t = trunc(x); return t < x ? t + 1 : t; }
double round(double x) {
    double t = trunc(x);
    if (fabs(x - t) >= 0.5) t += x > 0 ? 1 : -1;
    return t;
}
double rint(double x) {                                /* to nearest, ties to even (the FPU's mode) */
    if (!(fabs(x) < 4503599627370496.0)) return x;
    long long i;
    __asm__("cvtsd2si %1, %0" : "=r"(i) : "x"(x));
    return copysign((double)i, x);
}
float truncf(float x) { return (float)trunc(x); }
float floorf(float x) { return (float)floor(x); }
float ceilf(float x) { return (float)ceil(x); }
float roundf(float x) { return (float)round(x); }
float rintf(float x) { return (float)rint(x); }
long lrint(double x) { long i; __asm__("cvtsd2si %1, %0" : "=r"(i) : "x"(x)); return i; }
long lrintf(float x) { long i; __asm__("cvtss2si %1, %0" : "=r"(i) : "x"(x)); return i; }
long long llrint(double x) { return lrint(x); }
long long llrintf(float x) { return lrintf(x); }
long lround(double x) { return (long)round(x); }
long lroundf(float x) { return (long)round(x); }
long long llround(double x) { return (long long)round(x); }
long long llroundf(float x) { return (long long)round(x); }

double fmin(double x, double y) { return isnan(x) ? y : isnan(y) ? x : x < y ? x : y; }
double fmax(double x, double y) { return isnan(x) ? y : isnan(y) ? x : x > y ? x : y; }
float fminf(float x, float y) { return (float)fmin(x, y); }
float fmaxf(float x, float y) { return (float)fmax(x, y); }

double fmod(double x, double y) {
    double r;
    __asm__("1: fprem\n\tfnstsw %%ax\n\ttestw $0x400, %%ax\n\tjnz 1b\n\tfstp %%st(1)"
            : "=t"(r) : "0"(x), "u"(y) : "ax", "cc", "st(1)");
    return r;
}
float fmodf(float x, float y) { return (float)fmod(x, y); }

double scalbn(double x, int e) {
    double r;
    __asm__("fscale\n\tfstp %%st(1)" : "=t"(r) : "0"(x), "u"((double)e) : "st(1)");
    return r;
}
double ldexp(double x, int e) { return scalbn(x, e); }
float scalbnf(float x, int e) { return (float)scalbn(x, e); }
float ldexpf(float x, int e) { return (float)scalbn(x, e); }

double frexp(double x, int *e) {
    dbits b = { x };
    int ex = (int)(b.u >> 52 & 0x7FF);
    if (!ex) {                                         /* zero or subnormal */
        if (x == 0) { *e = 0; return x; }
        x = frexp(x * 18446744073709551616.0, e);       /* * 2^64 */
        *e -= 64;
        return x;
    }
    if (ex == 0x7FF) { *e = 0; return x; }
    *e = ex - 1022;
    b.u = (b.u & ~(0x7FFULL << 52)) | (1022ULL << 52);
    return b.d;
}
float frexpf(float x, int *e) { return (float)frexp(x, e); }
double modf(double x, double *ip) { *ip = trunc(x); return isinf(x) ? copysign(0, x) : x - *ip; }

/* 2^x on the FPU: split into a whole and a fraction (f2xm1 takes -1..1). */
double exp2(double x) {
    if (isnan(x)) return x;
    if (x > 1024) return HUGE_VAL;
    if (x < -1080) return 0;
    double i = rint(x), f = x - i, r;                  /* f in -0.5..0.5 */
    __asm__("f2xm1" : "=t"(r) : "0"(f));
    return scalbn(r + 1, (int)i);
}
double exp(double x) { return exp2(x * 1.4426950408889634074); }
double expm1(double x) { return fabs(x) < 1e-5 ? x + x * x / 2 + x * x * x / 6 : exp(x) - 1; }
double pow(double x, double y) {
    if (y == 0) return 1;
    if (x == 0) return y < 0 ? HUGE_VAL : 0;
    if (x < 0) {
        if (y != trunc(y)) return NAN;
        double r = exp2(y * log2(-x));
        return fmod(y, 2) != 0 ? -r : r;               /* odd power keeps the sign */
    }
    return exp2(y * log2(x));
}
float expf(float x) { return (float)exp(x); }
float exp2f(float x) { return (float)exp2(x); }
float powf(float x, float y) { return (float)pow(x, y); }

/* log2(x) = fyl2x with y = 1; other bases: y = log_b(2). */
static double fyl2x(double y, double x) {
    double r;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}
double log2(double x) {
    if (isnan(x) || x < 0) return NAN;
    if (x == 0) return -HUGE_VAL;
    if (isinf(x)) return x;
    return fyl2x(1.0, x);
}
double log(double x) { return log2(x) * M_LN2; }
double log10(double x) { return log2(x) * 0.30102999566398119521; }
double log1p(double x) { return fabs(x) < 1e-5 ? x - x * x / 2 : log(1 + x); }
float logf(float x) { return (float)log(x); }
float log2f(float x) { return (float)log2(x); }
float log10f(float x) { return (float)log10(x); }

double cbrt(double x) {
    if (x == 0 || isnan(x) || isinf(x)) return x;
    double r = exp2(log2(fabs(x)) / 3);
    r = r - (r * r * r - fabs(x)) / (3 * r * r);       /* one Newton step: exact cubes come out exact */
    return copysign(r, x);
}
float cbrtf(float x) { return (float)cbrt(x); }
double hypot(double x, double y) {
    x = fabs(x), y = fabs(y);
    if (isinf(x) || isinf(y)) return HUGE_VAL;
    if (x < y) { double t = x; x = y; y = t; }
    if (x == 0) return 0;
    y /= x;
    return x * sqrt(1 + y * y);
}
float hypotf(float x, float y) { return (float)hypot(x, y); }

/* fsin/fcos/fptan take |x| < 2^63; reduce first so big ones stay accurate. */
static double reduce(double x) { return fabs(x) > 1e6 ? fmod(x, 2 * M_PI) : x; }
double sin(double x) { double r; __asm__("fsin" : "=t"(r) : "0"(reduce(x))); return r; }
double cos(double x) { double r; __asm__("fcos" : "=t"(r) : "0"(reduce(x))); return r; }
double tan(double x) { double r; __asm__("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(reduce(x))); return r; }
float sinf(float x) { return (float)sin(x); }
float cosf(float x) { return (float)cos(x); }
float tanf(float x) { return (float)tan(x); }
double atan2(double y, double x) {
    double r;
    __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)");
    return r;
}
double atan(double x) { return atan2(x, 1); }
double asin(double x) { return atan2(x, sqrt((1 - x) * (1 + x))); }
double acos(double x) { return atan2(sqrt((1 - x) * (1 + x)), x); }
float atanf(float x) { return (float)atan(x); }
float atan2f(float y, float x) { return (float)atan2(y, x); }
double sinh(double x) { double e = exp(x); return (e - 1 / e) / 2; }
double cosh(double x) { double e = exp(x); return (e + 1 / e) / 2; }
double tanh(double x) {
    if (x > 20) return 1;
    if (x < -20) return -1;
    double e = exp(2 * x);
    return (e - 1) / (e + 1);
}
float tanhf(float x) { return (float)tanh(x); }

/* Abramowitz and Stegun 7.1.26 (error < 1.5e-7). */
double erf(double x) {
    double t = 1 / (1 + 0.3275911 * fabs(x));
    double y = 1 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return copysign(y, x);
}

/* ---- strtod: decimal and hex, inf, nan ------------------------------------------ */

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

double strtod(const char *s, char **end) {
    const char *p = s;
    while (*p == ' ' || (*p >= '\t' && *p <= '\r')) p++;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    double v = 0;
    const char *start = p;
    if (lower(p[0]) == 'i' && lower(p[1]) == 'n' && lower(p[2]) == 'f') {
        p += 3;
        if (lower(p[0]) == 'i' && lower(p[1]) == 'n' && lower(p[2]) == 'i' && lower(p[3]) == 't' && lower(p[4]) == 'y') p += 5;
        v = HUGE_VAL;
    } else if (lower(p[0]) == 'n' && lower(p[1]) == 'a' && lower(p[2]) == 'n') {
        p += 3;
        v = NAN;
    } else if (p[0] == '0' && lower(p[1]) == 'x') {
        p += 2;
        int e = 0, any = 0;
        for (;; p++) {
            int c = lower(*p), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
            any = 1;
        }
        if (*p == '.')
            for (p++;; p++) {
                int c = lower(*p), d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
                if (d < 0) break;
                v = v * 16 + d;
                e -= 4;
                any = 1;
            }
        if (!any) { p = start + 1; v = 0; }           /* just "0" */
        else if (lower(*p) == 'p') {
            const char *q = p + 1;
            int en = 0, eneg = 0;
            if (*q == '+' || *q == '-') eneg = *q++ == '-';
            if (*q >= '0' && *q <= '9') {
                while (*q >= '0' && *q <= '9') en = en * 10 + (*q++ - '0');
                e += eneg ? -en : en;
                p = q;
            }
        }
        v = ldexp(v, e);
    } else {
        int e = 0, any = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = 1; }
        if (*p == '.')
            for (p++; *p >= '0' && *p <= '9'; p++) { v = v * 10 + (*p - '0'); e--; any = 1; }
        if (!any) {
            if (end) *end = (char *)s;
            return 0;
        }
        if (lower(*p) == 'e') {
            const char *q = p + 1;
            int en = 0, eneg = 0;
            if (*q == '+' || *q == '-') eneg = *q++ == '-';
            if (*q >= '0' && *q <= '9') {
                while (*q >= '0' && *q <= '9') { if (en < 10000) en = en * 10 + (*q - '0'); q++; }
                e += eneg ? -en : en;
                p = q;
            }
        }
        if (e) v = e > 0 ? v * pow(10, e) : v / pow(10, -e);
        if (isinf(v)) errno = ERANGE;
    }
    if (end) *end = (char *)p;
    return neg ? -v : v;
}
float strtof(const char *s, char **end) { return (float)strtod(s, end); }
double atof(const char *s) { return strtod(s, 0); }
