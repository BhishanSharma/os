/* math.h - floating point maths (user/lib/math.c, from musl).
 * Programs built with -mgeneral-regs-only cannot use it; ports (FFmpeg,
 * minimp3) are built with SSE allowed. */
#ifndef MATH_H
#define MATH_H
#define NAN (__builtin_nanf(""))
#define INFINITY (__builtin_inff())
#define HUGE_VAL (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf_sign(x)
#define isfinite(x) __builtin_isfinite(x)
#define isnormal(x) __builtin_isnormal(x)
#define signbit(x) __builtin_signbit(x)
#define isgreater(x, y) __builtin_isgreater(x, y)
#define isless(x, y) __builtin_isless(x, y)
#define isunordered(x, y) __builtin_isunordered(x, y)
#define M_E 2.7182818284590452354
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

double fabs(double x);
float fabsf(float x);
double sqrt(double x);
float sqrtf(float x);
double floor(double x);
float floorf(float x);
double ceil(double x);
float ceilf(float x);
double trunc(double x);
float truncf(float x);
double round(double x);
float roundf(float x);
long lround(double x);
long lroundf(float x);
long long llround(double x);
long long llroundf(float x);
double rint(double x);
float rintf(float x);
long lrint(double x);
long lrintf(float x);
long long llrint(double x);
long long llrintf(float x);
double fmod(double x, double y);
float fmodf(float x, float y);
double fmin(double x, double y);
double fmax(double x, double y);
float fminf(float x, float y);
float fmaxf(float x, float y);
double copysign(double x, double y);
float copysignf(float x, float y);
double ldexp(double x, int e);
float ldexpf(float x, int e);
double scalbn(double x, int e);
float scalbnf(float x, int e);
double frexp(double x, int *e);
float frexpf(float x, int *e);
double modf(double x, double *ip);
double exp(double x);
float expf(float x);
double exp2(double x);
float exp2f(float x);
double expm1(double x);
double log(double x);
float logf(float x);
double log2(double x);
float log2f(float x);
double log10(double x);
float log10f(float x);
double log1p(double x);
double pow(double x, double y);
float powf(float x, float y);
double cbrt(double x);
float cbrtf(float x);
double hypot(double x, double y);
float hypotf(float x, float y);
double sin(double x);
float sinf(float x);
double cos(double x);
float cosf(float x);
double tan(double x);
float tanf(float x);
double asin(double x);
double acos(double x);
double atan(double x);
float atanf(float x);
double atan2(double y, double x);
float atan2f(float y, float x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
float tanhf(float x);
double erf(double x);
#endif
