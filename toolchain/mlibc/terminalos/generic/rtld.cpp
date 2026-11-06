// The dynamic linker's system calls (ld.so, built only for the shared C
// library): it opens and reads the libraries, maps memory for them and sets
// up the thread pointer. Nothing else: the rest of the port (sysdeps.cpp,
// net.cpp) needs the C library, which ld.so is loading.

#include <bits/ensure.h>
#include <errno.h>
#include <fcntl.h>
#include <mlibc/all-sysdeps.hpp>
#include <sys/mman.h>

#include <terminalos/syscall.h>

#include "common.hpp"

using tos::sc;
using tos::to_errno;

namespace mlibc {

void Sysdeps<LibcLog>::operator()(const char *message) {
	size_t n = 0;
	while (message[n])
		n++;
	sc(TOS_WRITE, 2, (long)message, (long)n);
	sc(TOS_WRITE, 2, (long)"\n", 1);
}

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("ld.so: panic");
	sc(TOS_EXIT, 134);
	__builtin_trap();
}

int Sysdeps<TcbSet>::operator()(void *pointer) {
	long r = sc(TOS_TCB_SET, (long)pointer);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Open>::operator()(const char *path, int flags, mode_t, int *fd) {
	if ((flags & O_ACCMODE) != O_RDONLY)
		return EACCES;
	long r = sc(TOS_OPEN, (long)path, TOS_OPEN_READ);
	if (r < 0)
		return to_errno(r);
	*fd = (int)r;
	return 0;
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
	long r = sc(TOS_READ, fd, (long)buf, (long)count);
	if (r < 0)
		return to_errno(r);
	*bytes_read = r;
	return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	long r = sc(TOS_SEEK, fd, (long)offset, whence);
	if (r < 0)
		return to_errno(r);
	*new_offset = r;
	return 0;
}

int Sysdeps<Close>::operator()(int fd) {
	long r = sc(TOS_CLOSE, fd);
	return r < 0 ? to_errno(r) : 0;
}

// Zeroed memory only: ld.so reads the segments in itself (MLIBC_MAP_DSO_SEGMENTS
// is off), at the addresses it picks (MAP_FIXED) or wherever the kernel says.
int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t, void **window) {
	if (!size || !(flags & MAP_ANONYMOUS || fd < 0))
		return EINVAL;
	long kprot = (prot & PROT_READ ? 1 : 0) | (prot & PROT_WRITE ? 2 : 0);
	long a = flags & MAP_FIXED ? sc(TOS_MMAPAT, (long)hint, (long)size, kprot ? kprot : 1, -1)
	                           : sc(TOS_MMAP, (long)size, kprot ? kprot : 1, -1);
	if (a <= 0)
		return ENOMEM;
	*window = (void *)a;
	return 0;
}

int Sysdeps<VmProtect>::operator()(void *, size_t, int) {
	return 0;                                       // (pages stay as mapped)
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	return sysdep<VmMap>(nullptr, (size + 4095) & ~size_t(4095), PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, pointer);
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	return sc(TOS_MUNMAP, (long)pointer, (long)((size + 4095) & ~size_t(4095))) < 0 ? EINVAL : 0;
}

// Its locks (as sysdeps.cpp).
pid_t Sysdeps<FutexTid>::operator()() {
	return (pid_t)sc(TOS_GETPID);
}

int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *) {
	long r = sc(TOS_FUTEX, TOS_FUTEX_WAIT, (long)pointer, expected);
	return r == TOS_ERR_AGAIN ? EAGAIN : r < 0 ? to_errno(r) : 0;
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool) {
	long r = sc(TOS_FUTEX, TOS_FUTEX_WAKE, (long)pointer, 0);
	return r < 0 ? to_errno(r) : 0;
}

// A library's identity (one file loaded once under two names): its path, as
// sysdeps.cpp's st_ino.
int Sysdeps<Stat>::operator()(fsfd_target target, int fd, const char *, int, struct stat *st) {
	char path[512];
	if (target != fsfd_target::fd || sc(TOS_FDPATH, fd, (long)path, sizeof(path)) <= 0)
		return ENOSYS;
	unsigned long h = 1469598103934665603ul;
	for (const char *p = path; *p; p++)
		h = (h ^ (unsigned char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p)) * 1099511628211ul;
	unsigned char *b = (unsigned char *)st;
	for (size_t i = 0; i < sizeof(*st); i++)
		b[i] = 0;
	st->st_dev = 1;
	st->st_ino = h ? h : 1;
	st->st_mode = S_IFREG | 0755;
	st->st_nlink = 1;
	return 0;
}

} // namespace mlibc
