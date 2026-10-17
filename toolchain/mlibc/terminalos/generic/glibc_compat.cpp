// Linux C library names that programs built for x86_64-unknown-linux-gnu
// call (Rust's standard library, in Ladybird): the 64-bit file calls (the
// same as the plain ones on x86_64), glibc's extras, and syscall() for the
// Linux system calls Rust makes itself (futex, getrandom, gettid). Any other
// Linux call fails with ENOSYS, and Rust falls back to the portable way.
// (No <sys/stat.h> or <fcntl.h> here: they may define stat64 as stat.)

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <terminalos/syscall.h>

struct stat;

extern "C" {

int stat(const char *path, struct stat *st);
int fstatat(int dirfd, const char *path, struct stat *st, int flags);
int openat(int dirfd, const char *path, int flags, ...);

int stat64(const char *path, struct stat *st) {
	return stat(path, st);
}

int fstatat64(int dirfd, const char *path, struct stat *st, int flags) {
	return fstatat(dirfd, path, st, flags);
}

int openat64(int dirfd, const char *path, int flags, ...) {
	va_list ap;
	va_start(ap, flags);
	unsigned int mode = va_arg(ap, unsigned int);   // (read whether given or not, as glibc does)
	va_end(ap);
	return openat(dirfd, path, flags, mode);
}

// Callers copy the slow way when this fails.
long sendfile64(int, int, long *, size_t) {
	errno = ENOSYS;
	return -1;
}

int __xpg_strerror_r(int e, char *buf, size_t size) {
	const char *s = strerror(e);
	size_t n = strlen(s);
	if (!size)
		return ERANGE;
	if (n >= size) {
		memcpy(buf, s, size - 1);
		buf[size - 1] = 0;
		return ERANGE;
	}
	memcpy(buf, s, n + 1);
	return 0;
}

const char *gnu_get_libc_version(void) {
	return "2.35";
}

int __res_init(void) {
	return 0;
}

// Linux system call numbers (x86_64).
enum { L_GETPID = 39, L_GETTID = 186, L_FUTEX = 202, L_GETRANDOM = 318 };
enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_WAIT_BITSET = 9, FUTEX_WAKE_BITSET = 10 };

long syscall(long n, ...) {
	va_list ap;
	va_start(ap, n);
	long a = va_arg(ap, long), b = va_arg(ap, long), c = va_arg(ap, long);
	va_end(ap);
	switch (n) {
	case L_GETPID:
	case L_GETTID:
		return tos_syscall(TOS_GETPID, 0, 0, 0, 0);
	case L_FUTEX: {
		// A wait ends on a wake or after at most 100 ms; callers re-check
		// the word and their deadline, as after any spurious wake.
		int op = (int)b & 127;
		if (op == FUTEX_WAIT || op == FUTEX_WAIT_BITSET) {
			long r = tos_syscall(TOS_FUTEX, TOS_FUTEX_WAIT, a, (int)c, 0);
			if (r == TOS_ERR_AGAIN) {
				errno = EAGAIN;
				return -1;
			}
			return 0;
		}
		if (op == FUTEX_WAKE || op == FUTEX_WAKE_BITSET) {
			tos_syscall(TOS_FUTEX, TOS_FUTEX_WAKE, a, 0, 0);   // (it wakes every waiter)
			return 0;
		}
		break;
	}
	case L_GETRANDOM: {
		unsigned char *out = (unsigned char *)a;
		size_t size = (size_t)b;
		while (size) {
			size_t chunk = size < 256 ? size : 256;
			if (getentropy(out, chunk) != 0)
				return -1;
			out += chunk;
			size -= chunk;
		}
		return b;
	}
	}
	errno = ENOSYS;
	return -1;
}

}
