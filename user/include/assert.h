/* assert.h */
#ifndef ASSERT_H
#define ASSERT_H
void __assert_fail(const char *expr, const char *file, int line, const char *func) __attribute__((noreturn));
#endif
#undef assert
#ifdef NDEBUG
#define assert(x) ((void)0)
#else
#define assert(x) ((x) ? (void)0 : __assert_fail(#x, __FILE__, __LINE__, __func__))
#endif
#if !defined(__cplusplus) && !defined(static_assert)
#define static_assert _Static_assert
#endif
