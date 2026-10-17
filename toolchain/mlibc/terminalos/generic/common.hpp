// Shared by the port's files: system calls, error numbers, and the file
// descriptors the port makes itself (sockets, built-in files).
#pragma once

#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <terminalos/syscall.h>

namespace tos {

inline long sc(long n, long a = 0, long b = 0, long c = 0, long d = 0) {
	return tos_syscall(n, a, b, c, d);
}

inline int to_errno(long r) {
	switch (r) {
	case TOS_ERR_BADCALL: return ENOSYS;
	case TOS_ERR_FAULT: return EFAULT;
	case TOS_ERR_BADFD: return EBADF;
	case TOS_ERR_NOENT: return ENOENT;
	case TOS_ERR_NOMEM: return ENOMEM;
	case TOS_ERR_MFILE: return EMFILE;
	case TOS_ERR_IO: return EIO;
	case TOS_ERR_PERM: return EACCES;
	case TOS_ERR_NOEXEC: return ENOEXEC;
	case TOS_ERR_AGAIN: return EAGAIN;
	case TOS_ERR_CHILD: return ECHILD;
	case TOS_ERR_NOUSER: return EINVAL;
	case TOS_ERR_AUTH: return EPERM;
	case TOS_ERR_SRCH: return ESRCH;
	case TOS_ERR_TIMEOUT: return ETIMEDOUT;
	case TOS_ERR_REFUSED: return ECONNREFUSED;
	case TOS_ERR_RESET: return ECONNRESET;
	case TOS_ERR_CLOSED: return EPIPE;
	case TOS_ERR_NOROUTE: return ENETUNREACH;
	case TOS_ERR_INUSE: return EADDRINUSE;
	case TOS_ERR_PIPE: return EPIPE;
	case TOS_ERR_WOULDBLOCK: return EAGAIN;
	default: return EIO;
	}
}

// File descriptors: the kernel's are below 256 (files, pipes, local sockets);
// the port's are above.
constexpr int SOCK_BASE = 256, MAX_SOCKS = 64;    // 256..319: sockets
constexpr int VF_BASE = 320, MAX_VFILES = 32;     // 320..351: /dev/null, /etc/services...
constexpr int DIR_BASE = 4096;                    // open directories (never in select())

bool is_socket(int fd);
// FD_CLOEXEC of a kernel descriptor (exec closes it there).
inline void set_cloexec(int fd, bool on) {
	if (fd >= 0 && fd < SOCK_BASE)
		sc(TOS_FDFLAGS, fd, on ? 1 : 0);
}
bool is_vfile(int fd);

// Sockets and built-in files (net.cpp).
int special_read(int fd, void *buf, size_t count, ssize_t *done);
int special_write(int fd, const void *buf, size_t count, ssize_t *done);
int special_close(int fd);
int special_seek(int fd, off_t offset, int whence, off_t *pos);
int special_stat(int fd, struct stat *st);

// Paths: a built-in file opens here (1: done, *fd set or error in *err);
// the certificate bundle's usual names lead to the real file.
int open_builtin(const char *path, int flags, int *fd, int *err);
bool builtin_exists(const char *path, struct stat *st);
const char *redirect(const char *path);

} // namespace tos
