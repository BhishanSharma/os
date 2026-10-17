// mlibc on Terminal OS: the C library's needs, in terms of the kernel's
// system calls (include/terminalos/syscall.h).
//
// The kernel's file interface is smaller than POSIX: files open for reading,
// for writing (created or emptied) or for appending; no fstat, no rename, no
// times. The gaps are filled here where it is cheap (fstat by the path the
// file was opened with, directories as a listing taken at opendir) and are
// ENOSYS otherwise.

#include <bits/ensure.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mlibc/all-sysdeps.hpp>
#include <mlibc/debug.hpp>
#include <mlibc/tcb.hpp>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <terminalos/syscall.h>

#include "common.hpp"

extern "C" void __mlibc_thread_entry();

namespace {

using tos::sc;
using tos::to_errno;
using tos::DIR_BASE;

// ---- What the kernel does not remember: each fd's path, open directories ----

constexpr int MAX_FDS = 256;      // the kernel's limit is lower
constexpr int MAX_DIRS = 64;

char *fd_path[MAX_FDS];

struct OpenDirectory {
	bool used;
	char *path;
	tos_dirent *entries;
	int count, next;
};
OpenDirectory dirs[MAX_DIRS];

// An absolute path for `path` (the kernel resolves relative ones against the
// current directory; stat by fd needs to know which).
char *absolute(const char *path) {
	if (path[0] == '/')
		return strdup(path);
	char cwd[256];
	long n = sc(TOS_GETCWD, (long)cwd, sizeof(cwd));
	if (n < 0)
		return strdup(path);
	size_t len = strlen(cwd) + strlen(path) + 2;
	char *out = (char *)malloc(len);
	if (!out)
		return nullptr;
	strcpy(out, cwd);
	if (len > 2 && out[strlen(out) - 1] != '/')
		strcat(out, "/");
	strcat(out, path);
	return out;
}

void remember(int fd, const char *path) {
	if (fd < 0 || fd >= MAX_FDS)
		return;
	free(fd_path[fd]);
	fd_path[fd] = absolute(path);
}

void forget(int fd) {
	if (fd < 0 || fd >= MAX_FDS)
		return;
	free(fd_path[fd]);
	fd_path[fd] = nullptr;
}

unsigned long path_hash(const char *p) {
	unsigned long h = 1469598103934665603ul;
	for (; *p; p++)
		h = (h ^ (unsigned char)(*p >= 'a' && *p <= 'z' ? *p - 32 : *p)) * 1099511628211ul;   // FAT: case-blind
	return h ? h : 1;
}

int fill_stat(const char *path, struct stat *st) {
	tos_stat ks;
	long r = sc(TOS_STAT, (long)path, (long)&ks);
	if (r < 0)
		return to_errno(r);
	memset(st, 0, sizeof(*st));
	st->st_dev = 1;
	st->st_ino = path_hash(path);
	st->st_nlink = 1;
	st->st_mode = ks.is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
	st->st_size = ks.size;
	st->st_blksize = 4096;
	st->st_blocks = (ks.size + 511) / 512;
	return 0;
}

int open_directory(const char *path, int *handle) {
	for (int i = 0; i < MAX_DIRS; i++) {
		if (dirs[i].used)
			continue;
		int cap = 64;
		tos_dirent *list = nullptr;
		long n;
		for (;;) {                                  // grow until the whole folder fits
			list = (tos_dirent *)realloc(list, cap * sizeof(tos_dirent));
			if (!list)
				return ENOMEM;
			n = sc(TOS_LISTDIR, (long)path, (long)list, cap);
			if (n < 0) {
				free(list);
				return to_errno(n);
			}
			if (n < cap)
				break;
			cap *= 2;
		}
		dirs[i] = {true, absolute(path), list, (int)n, 0};
		*handle = DIR_BASE + i;
		return 0;
	}
	return EMFILE;
}

OpenDirectory *directory(int handle) {
	int i = handle - DIR_BASE;
	if (i < 0 || i >= MAX_DIRS || !dirs[i].used)
		return nullptr;
	return &dirs[i];
}

// ---- Signals: the kernel calls trampoline(sig, handler, frame) ---------------

constexpr int SIGNALS = 65;   // 1..64, as Linux
struct sigaction actions[SIGNALS];

void signal_trampoline(int sig, void (*)(int), void *frame) {
	struct sigaction &a = actions[sig > 0 && sig < SIGNALS ? sig : 0];
	if (a.sa_flags & SA_SIGINFO) {
		siginfo_t info;
		memset(&info, 0, sizeof(info));
		info.si_signo = sig;
		a.sa_sigaction(sig, &info, nullptr);
	} else if (a.sa_handler != SIG_IGN && a.sa_handler != SIG_DFL) {
		a.sa_handler(sig);
	}
	sc(TOS_SIGRETURN, (long)frame);
	__builtin_trap();
}

// ---- Time --------------------------------------------------------------------

long long days_from_civil(int y, int m, int d) {
	y -= m <= 2;
	long long era = (y >= 0 ? y : y - 399) / 400;
	unsigned yoe = (unsigned)(y - era * 400);
	unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
	unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
	return era * 146097 + (long long)doe - 719468;
}

long long boot_epoch_ms = -1;   // the real time when uptime was 0

} // namespace

extern "C" void __mlibc_thread_trampoline(void *(*fn)(void *), Tcb *tcb, void *arg) {
	if (mlibc::sysdep<TcbSet>(tcb))
		__ensure(!"failed to set the tcb of a new thread");
	while (__atomic_load_n(&tcb->tid, __ATOMIC_RELAXED) == 0)
		mlibc::sysdep<FutexWait>(&tcb->tid, 0, nullptr);
	__atomic_fetch_or(&tcb->cancelBits, tcbCancelEnableBit, __ATOMIC_RELAXED);
	tcb->invokeThreadFunc(reinterpret_cast<void *>(fn), arg);
	mlibc::thread_exit(tcb->returnValue);
}

namespace mlibc {

void Sysdeps<LibcLog>::operator()(const char *message) {
	sc(TOS_WRITE, 2, (long)message, strlen(message));
	sc(TOS_WRITE, 2, (long)"\n", 1);
}

void Sysdeps<LibcPanic>::operator()() {
	sysdep<LibcLog>("mlibc: panic");
	sc(TOS_EXIT, 134);
	__builtin_trap();
}

void Sysdeps<Exit>::operator()(int status) {
	sc(TOS_EXIT, status);
	__builtin_unreachable();
}

void Sysdeps<ThreadExit>::operator()() {
	sc(TOS_THREAD_EXIT, 0);
	__builtin_unreachable();
}

// x86_64: %fs points at the thread control block.
int Sysdeps<TcbSet>::operator()(void *pointer) {
	long r = sc(TOS_TCB_SET, (long)pointer);
	return r < 0 ? to_errno(r) : 0;
}

pid_t Sysdeps<FutexTid>::operator()() {
	return (pid_t)sc(TOS_GETPID);   // every thread is its own task, with its own id
}

// The kernel's wait ends after at most 100 ms (or a wake); a timed wait loops
// until its deadline, an untimed one returns (callers re-check the word).
int Sysdeps<FutexWait>::operator()(int *pointer, int expected, const struct timespec *time) {
	if (!time) {
		long r = sc(TOS_FUTEX, TOS_FUTEX_WAIT, (long)pointer, expected);
		return r == TOS_ERR_AGAIN ? EAGAIN : r < 0 ? to_errno(r) : 0;
	}
	unsigned long start = (unsigned long)sc(TOS_UPTIME);
	unsigned long limit = (unsigned long)time->tv_sec * 1000 + (unsigned long)time->tv_nsec / 1000000;
	for (;;) {
		long r = sc(TOS_FUTEX, TOS_FUTEX_WAIT, (long)pointer, expected);
		if (r == TOS_ERR_AGAIN)
			return EAGAIN;
		if (r < 0)
			return to_errno(r);
		if (__atomic_load_n(pointer, __ATOMIC_RELAXED) != expected)
			return 0;
		if ((unsigned long)sc(TOS_UPTIME) - start >= limit)
			return ETIMEDOUT;
	}
}

int Sysdeps<FutexWake>::operator()(int *pointer, bool) {
	long r = sc(TOS_FUTEX, TOS_FUTEX_WAKE, (long)pointer, 0);   // (it wakes every waiter)
	return r < 0 ? to_errno(r) : 0;
}

// ---- Memory ----------------------------------------------------------------------

int Sysdeps<VmMap>::operator()(void *hint, size_t size, int prot, int flags, int fd, off_t offset, void **window) {
	(void)hint;
	if (flags & MAP_FIXED)
		return ENOMEM;                              // (the kernel picks the address)
	if (!size)
		return EINVAL;
	long kprot = (prot & PROT_READ ? 1 : 0) | (prot & PROT_WRITE ? 2 : 0);
	if (flags & MAP_ANONYMOUS || fd < 0) {
		long a = sc(TOS_MMAP, (long)size, kprot ? kprot : 1, -1);
		if (a <= 0)
			return ENOMEM;
		*window = (void *)a;
		return 0;
	}
	if (flags & MAP_SHARED && prot & PROT_WRITE)
		return ENOTSUP;                             // no writing back to the file
	// A private copy of the file: zeroed memory, then the bytes read in.
	long a = sc(TOS_MMAP, (long)size, 3, -1);
	if (a <= 0)
		return ENOMEM;
	long pos = sc(TOS_SEEK, fd, 0, SEEK_CUR);
	if (sc(TOS_SEEK, fd, offset, SEEK_SET) < 0) {
		sc(TOS_MUNMAP, a, (long)size);
		return EACCES;
	}
	for (size_t done = 0; done < size;) {
		long n = sc(TOS_READ, fd, a + (long)done, (long)(size - done));
		if (n <= 0)
			break;
		done += n;
	}
	if (pos >= 0)
		sc(TOS_SEEK, fd, pos, SEEK_SET);
	*window = (void *)a;
	return 0;
}

int Sysdeps<VmUnmap>::operator()(void *pointer, size_t size) {
	long r = sc(TOS_MUNMAP, (long)pointer, (long)size);
	return r < 0 ? EINVAL : 0;
}

int Sysdeps<VmProtect>::operator()(void *, size_t, int) {
	return 0;                                       // (pages stay as mapped)
}

int Sysdeps<AnonAllocate>::operator()(size_t size, void **pointer) {
	return sysdep<VmMap>(nullptr, (size + 4095) & ~size_t(4095), PROT_READ | PROT_WRITE,
	                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, pointer);
}

int Sysdeps<AnonFree>::operator()(void *pointer, size_t size) {
	return sysdep<VmUnmap>(pointer, (size + 4095) & ~size_t(4095));
}

// ---- Threads -------------------------------------------------------------------

int Sysdeps<PrepareStack>::operator()(void **stack, void *entry, void *arg, void *tcb, size_t *stack_size,
                                      size_t *guard_size, void **stack_base) {
	*guard_size = 0;
	*stack_size = *stack_size ? *stack_size : 0x200000;
	if (!*stack) {
		void *base;
		if (int e = sysdep<AnonAllocate>(*stack_size, &base); e)
			return e;
		*stack_base = base;
	} else {
		*stack_base = (char *)*stack - *stack_size;
	}
	void **sp = (void **)((char *)*stack_base + *stack_size);
	*--sp = nullptr;                                // (keeps the three below 16-byte aligned)
	*--sp = arg;
	*--sp = tcb;
	*--sp = entry;
	*stack = sp;
	return 0;
}

// The kernel starts the thread at __mlibc_thread_entry with rdi = the stack
// PrepareStack made: entry, tcb, arg.
int Sysdeps<Clone>::operator()(void *, pid_t *pid_out, void *stack) {
	long r = sc(TOS_THREAD, (long)__mlibc_thread_entry, (long)stack, (long)stack);
	if (r < 0)
		return to_errno(r);
	*pid_out = (pid_t)r;
	return 0;
}

// ---- Files -------------------------------------------------------------------------

int Sysdeps<Open>::operator()(const char *path, int flags, mode_t, int *fd) {
	int err;
	if (tos::open_builtin(path, flags, fd, &err))
		return err;
	path = tos::redirect(path);
	tos_stat ks;
	bool exists = sc(TOS_STAT, (long)path, (long)&ks) == 0;
	if (exists && ks.is_dir) {
		if ((flags & O_ACCMODE) != O_RDONLY)
			return EISDIR;
		return open_directory(path, fd);
	}
	if (flags & O_DIRECTORY)
		return exists ? ENOTDIR : ENOENT;
	if (exists && (flags & O_CREAT) && (flags & O_EXCL))
		return EEXIST;
	long mode;
	if ((flags & O_ACCMODE) == O_RDONLY) {
		mode = TOS_OPEN_READ;
	} else {
		if (!exists && !(flags & O_CREAT))
			return ENOENT;
		if (flags & O_APPEND)
			mode = TOS_OPEN_APPEND;
		else if ((flags & O_ACCMODE) == O_RDWR && exists && !(flags & O_TRUNC))
			mode = TOS_OPEN_READ;                   // (reading works; writes fail with EBADF)
		else
			mode = TOS_OPEN_WRITE;
	}
	long r = sc(TOS_OPEN, (long)path, mode);
	if (r < 0)
		return to_errno(r);
	*fd = (int)r;
	remember(*fd, path);
	return 0;
}

int Sysdeps<Openat>::operator()(int dirfd, const char *path, int flags, mode_t mode, int *fd) {
	if (dirfd != AT_FDCWD && path[0] != '/') {
		OpenDirectory *d = directory(dirfd);
		if (!d)
			return dirfd >= 0 && dirfd < MAX_FDS && fd_path[dirfd] ? ENOTDIR : EBADF;
		char *full = (char *)malloc(strlen(d->path) + strlen(path) + 2);
		if (!full)
			return ENOMEM;
		strcpy(full, d->path);
		strcat(full, "/");
		strcat(full, path);
		int e = sysdep<Open>(full, flags, mode, fd);
		free(full);
		return e;
	}
	return sysdep<Open>(path, flags, mode, fd);
}

int Sysdeps<Read>::operator()(int fd, void *buf, size_t count, ssize_t *bytes_read) {
	if (tos::is_socket(fd) || tos::is_vfile(fd))
		return tos::special_read(fd, buf, count, bytes_read);
	if (directory(fd))
		return EISDIR;
	long r = sc(TOS_READ, fd, (long)buf, (long)count);
	if (r < 0)
		return to_errno(r);
	*bytes_read = r;
	return 0;
}

int Sysdeps<Write>::operator()(int fd, const void *buf, size_t count, ssize_t *bytes_written) {
	if (tos::is_socket(fd) || tos::is_vfile(fd))
		return tos::special_write(fd, buf, count, bytes_written);
	long r = sc(TOS_WRITE, fd, (long)buf, (long)count);
	if (r < 0)
		return to_errno(r);
	*bytes_written = r;
	return 0;
}

int Sysdeps<Seek>::operator()(int fd, off_t offset, int whence, off_t *new_offset) {
	if (tos::is_socket(fd) || tos::is_vfile(fd))
		return tos::special_seek(fd, offset, whence, new_offset);
	if (fd >= 0 && fd <= 2 && !fd_path[fd])
		return ESPIPE;
	long r = sc(TOS_SEEK, fd, (long)offset, whence);
	if (r < 0)
		return r == TOS_ERR_BADFD ? ESPIPE : to_errno(r);
	*new_offset = r;
	return 0;
}

int Sysdeps<Close>::operator()(int fd) {
	if (tos::is_socket(fd) || tos::is_vfile(fd))
		return tos::special_close(fd);
	if (OpenDirectory *d = directory(fd)) {
		free(d->path);
		free(d->entries);
		*d = {};
		return 0;
	}
	long r = sc(TOS_CLOSE, fd);
	forget(fd);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Stat>::operator()(fsfd_target target, int fd, const char *path, int, struct stat *st) {
	switch (target) {
	case fsfd_target::path:
		if (tos::builtin_exists(path, st))
			return 0;
		return fill_stat(tos::redirect(path), st);
	case fsfd_target::fd_path:
		if (fd == AT_FDCWD || path[0] == '/')
			return fill_stat(path, st);
		if (OpenDirectory *d = directory(fd)) {
			char *full = (char *)malloc(strlen(d->path) + strlen(path) + 2);
			if (!full)
				return ENOMEM;
			strcpy(full, d->path);
			strcat(full, "/");
			strcat(full, path);
			int e = fill_stat(full, st);
			free(full);
			return e;
		}
		return EBADF;
	case fsfd_target::fd:
		if (tos::is_socket(fd) || tos::is_vfile(fd))
			return tos::special_stat(fd, st);
		if (OpenDirectory *d = directory(fd))
			return fill_stat(d->path, st);
		if (fd >= 0 && fd < MAX_FDS && fd_path[fd])
			return fill_stat(fd_path[fd], st);
		if (fd >= 0 && fd <= 2) {                   // the console (or a pipe)
			memset(st, 0, sizeof(*st));
			st->st_mode = S_IFCHR | 0620;
			st->st_nlink = 1;
			st->st_blksize = 4096;
			return 0;
		}
		return EBADF;
	default:
		return ENOSYS;
	}
}

int Sysdeps<Isatty>::operator()(int fd) {
	if (fd >= 0 && fd <= 2 && !fd_path[fd])
		return 0;
	return ENOTTY;
}

int Sysdeps<OpenDir>::operator()(const char *path, int *handle) {
	return open_directory(path, handle);
}

// One entry per call (readdir asks again when the buffer is used up).
int Sysdeps<ReadEntries>::operator()(int handle, void *buffer, size_t max_size, size_t *bytes_read) {
	OpenDirectory *d = directory(handle);
	if (!d)
		return EBADF;
	if (d->next >= d->count) {
		*bytes_read = 0;
		return 0;
	}
	const tos_dirent &e = d->entries[d->next];
	size_t name_len = strlen(e.name);
	size_t reclen = (offsetof(struct dirent, d_name) + name_len + 1 + 7) & ~size_t(7);
	if (reclen > max_size)
		return EINVAL;
	struct dirent *out = (struct dirent *)buffer;
	out->d_ino = path_hash(e.name) ^ (unsigned long)(d->next + 1);
	out->d_off = d->next + 1;
	out->d_reclen = (unsigned short)reclen;
	out->d_type = e.is_dir ? DT_DIR : DT_REG;
	memcpy(out->d_name, e.name, name_len + 1);
	d->next++;
	*bytes_read = reclen;
	return 0;
}

int Sysdeps<Access>::operator()(const char *path, int) {
	if (tos::builtin_exists(path, nullptr))
		return 0;
	tos_stat ks;
	long r = sc(TOS_STAT, (long)tos::redirect(path), (long)&ks);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<GetCwd>::operator()(char *buffer, size_t size) {
	char cwd[256];
	long n = sc(TOS_GETCWD, (long)cwd, sizeof(cwd));
	if (n < 0)
		return to_errno(n);
	size_t len = strlen(cwd);
	if (len + 1 > size)
		return ERANGE;
	memcpy(buffer, cwd, len + 1);
	return 0;
}

int Sysdeps<Chdir>::operator()(const char *path) {
	long r = sc(TOS_CHDIR, (long)path);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Mkdir>::operator()(const char *path, mode_t) {
	tos_stat ks;
	if (sc(TOS_STAT, (long)path, (long)&ks) == 0)
		return EEXIST;
	long r = sc(TOS_MKDIR, (long)path);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Rmdir>::operator()(const char *path) {
	long r = sc(TOS_UNLINK, (long)path);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Unlinkat>::operator()(int dirfd, const char *path, int) {
	if (dirfd != AT_FDCWD && path[0] != '/')
		return ENOSYS;
	long r = sc(TOS_UNLINK, (long)path);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Pipe>::operator()(int *fds, int) {
	long r = sc(TOS_PIPE, (long)fds);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Dup>::operator()(int fd, int, int *newfd) {
	if (tos::is_socket(fd) || tos::is_vfile(fd) || directory(fd))
		return EINVAL;                              // (only the kernel's own descriptors)
	long r = sc(TOS_DUP, fd);
	if (r < 0)
		return to_errno(r);
	*newfd = (int)r;
	if (fd >= 0 && fd < MAX_FDS && fd_path[fd])
		remember(*newfd, fd_path[fd]);
	return 0;
}

int Sysdeps<Dup2>::operator()(int fd, int, int newfd) {
	long r = sc(TOS_DUP2, fd, newfd);
	if (r < 0)
		return to_errno(r);
	forget(newfd);
	if (fd >= 0 && fd < MAX_FDS && fd_path[fd])
		remember(newfd, fd_path[fd]);
	return 0;
}

// ---- Time -------------------------------------------------------------------------------

int Sysdeps<ClockGet>::operator()(int clock, time_t *secs, long *nanos) {
	long long ms = (unsigned long)sc(TOS_UPTIME);
	if (clock == CLOCK_REALTIME || clock == CLOCK_REALTIME_COARSE) {
		if (boot_epoch_ms < 0) {
			tos_time t;
			if (sc(TOS_TIME, (long)&t, 1) < 0)
				return EIO;
			long long s = days_from_civil(t.year, t.month, t.day) * 86400 + t.hour * 3600 + t.minute * 60 + t.second;
			boot_epoch_ms = s * 1000 - ms;
		}
		ms += boot_epoch_ms;
	}
	*secs = ms / 1000;
	*nanos = (ms % 1000) * 1000000;
	return 0;
}

int Sysdeps<ClockGetres>::operator()(int, time_t *secs, long *nanos) {
	*secs = 0;
	*nanos = 1000000;
	return 0;
}

int Sysdeps<Sleep>::operator()(time_t *secs, long *nanos) {
	long ms = (long)*secs * 1000 + (*nanos + 999999) / 1000000;
	if (ms > 0)
		sc(TOS_SLEEP, ms);
	else
		sc(TOS_YIELD);
	*secs = 0;
	*nanos = 0;
	return 0;
}

void Sysdeps<Yield>::operator()() {
	sc(TOS_YIELD);
}

// ---- Programs ---------------------------------------------------------------------------

pid_t Sysdeps<GetPid>::operator()() { return (pid_t)sc(TOS_GETPID); }
pid_t Sysdeps<GetTid>::operator()() { return (pid_t)sc(TOS_GETPID); }
pid_t Sysdeps<GetPpid>::operator()() { return 1; }

uid_t Sysdeps<GetUid>::operator()() {
	tos_user u;
	return sc(TOS_GETUSER, (long)&u) < 0 ? 0 : u.uid;
}
uid_t Sysdeps<GetEuid>::operator()() { return sysdep<GetUid>(); }
gid_t Sysdeps<GetGid>::operator()() { return sysdep<GetUid>(); }
gid_t Sysdeps<GetEgid>::operator()() { return sysdep<GetUid>(); }

int Sysdeps<Fork>::operator()(pid_t *child) {
	long r = sc(TOS_FORK);
	if (r < 0)
		return to_errno(r);
	*child = (pid_t)r;
	return 0;
}

int Sysdeps<Execve>::operator()(const char *path, char *const argv[], char *const[]) {
	long r = sc(TOS_EXEC, (long)path, (long)argv);
	return to_errno(r);
}

int Sysdeps<Waitpid>::operator()(pid_t pid, int *status, int flags, struct rusage *, pid_t *ret_pid) {
	int code = 0;
	long r = sc(TOS_WAIT, pid, (long)&code, flags & WNOHANG ? TOS_WAIT_NOHANG : 0);
	if (r < 0)
		return to_errno(r);
	if (status)
		*status = (code & 0xff) << 8;               // as exited normally
	*ret_pid = (pid_t)r;
	return 0;
}

int Sysdeps<Kill>::operator()(pid_t pid, int sig) {
	if (sig == 0)
		return 0;
	long r = sc(TOS_SIGSEND, pid, sig);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Sigaction>::operator()(int sig, const struct sigaction *__restrict act,
                                   struct sigaction *__restrict oldact) {
	if (sig <= 0 || sig >= SIGNALS)
		return EINVAL;
	if (oldact)
		*oldact = actions[sig];
	if (!act)
		return 0;
	if (sig == SIGKILL || sig == SIGSTOP)
		return EINVAL;
	actions[sig] = *act;
	long handler = act->sa_handler == SIG_DFL ? 0 : 1;   // the trampoline calls the real one
	long r = sc(TOS_SIGNAL, sig, handler, (long)signal_trampoline);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Sigprocmask>::operator()(int, const sigset_t *__restrict, sigset_t *__restrict retrieve) {
	if (retrieve)
		sigemptyset(retrieve);
	return 0;                                       // (no masks: signals arrive at once)
}

int Sysdeps<Uname>::operator()(struct utsname *buf) {
	tos_uname u;
	memset(buf, 0, sizeof(*buf));
	if (sc(TOS_UNAME, (long)&u) < 0)
		return EIO;
	strncpy(buf->sysname, "TerminalOS", sizeof(buf->sysname) - 1);
	strncpy(buf->nodename, u.hostname, sizeof(buf->nodename) - 1);
	strncpy(buf->release, u.release, sizeof(buf->release) - 1);
	strncpy(buf->version, u.sysname, sizeof(buf->version) - 1);
	strncpy(buf->machine, "x86_64", sizeof(buf->machine) - 1);
	return 0;
}

int Sysdeps<GetHostname>::operator()(char *buffer, size_t bufsize) {
	tos_uname u;
	if (sc(TOS_UNAME, (long)&u) < 0)
		return EIO;
	if (strlen(u.hostname) + 1 > bufsize)
		return ENAMETOOLONG;
	strcpy(buffer, u.hostname);
	return 0;
}

// RDRAND where the processor has it, else a mix of the clock.
int Sysdeps<GetEntropy>::operator()(void *buffer, size_t length) {
	unsigned a, b, c, d;
	__asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
	bool rdrand = c & (1u << 30);
	static unsigned long state = 0x9E3779B97F4A7C15ul;
	unsigned char *out = (unsigned char *)buffer;
	for (size_t i = 0; i < length; i += 8) {
		unsigned long v = 0;
		unsigned char ok = 0;
		for (int tries = 0; rdrand && !ok && tries < 10; tries++)
			__asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
		if (!ok) {
			unsigned lo, hi;
			__asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
			state ^= ((unsigned long)hi << 32 | lo) + (unsigned long)sc(TOS_UPTIME);
			state ^= state << 13;
			state ^= state >> 7;
			state ^= state << 17;
			v = state;
		}
		size_t n = length - i < 8 ? length - i : 8;
		memcpy(out + i, &v, n);
	}
	return 0;
}

// No interval timers in the kernel yet: alarm() and setitimer() are accepted
// and never fire (curl uses one only to bound a DNS lookup, which here does
// not hang).
int Sysdeps<SetItimer>::operator()(int, const struct itimerval *, struct itimerval *old) {
	if (old)
		memset(old, 0, sizeof(*old));
	return 0;
}

int Sysdeps<GetItimer>::operator()(int, struct itimerval *value) {
	memset(value, 0, sizeof(*value));
	return 0;
}

int Sysdeps<Sysconf>::operator()(int num, long *ret) {
	switch (num) {
	case _SC_NPROCESSORS_ONLN:
	case _SC_NPROCESSORS_CONF: {
		long n = sc(TOS_SYSTEM, TOS_SYSTEM_CPUS);
		*ret = n > 0 ? n : 1;
		return 0;
	}
	case _SC_PAGESIZE:
		*ret = 4096;
		return 0;
	default:
		return EINVAL;                              // mlibc's table answers the rest
	}
}

} // namespace mlibc
