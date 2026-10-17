// Sockets, poll/select and built-in files for mlibc on Terminal OS.
//
// TCP sockets are the kernel's (connect, listen, accept, send, recv,
// sockinfo); here they get file descriptors, O_NONBLOCK, poll() and the BSD
// calls. There is no UDP for programs: a datagram socket can only ask DNS
// questions (port 53), and the answer comes from the kernel's resolver, so
// mlibc's getaddrinfo works unchanged. Local (Unix-domain) stream sockets are
// the kernel's own descriptors, passing files with SCM_RIGHTS.
// /etc/services, /etc/hosts,
// /etc/resolv.conf, /dev/null, /dev/zero and /dev/urandom are built in.

#include "common.hpp"

#include <fcntl.h>
#include <mlibc/all-sysdeps.hpp>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>

namespace tos {
namespace {

uint16_t be16(uint16_t v) { return __builtin_bswap16(v); }

// ---- Sockets -----------------------------------------------------------------------

struct Sock {
	bool used;
	int type;                // SOCK_STREAM or SOCK_DGRAM
	int k;                   // the kernel's socket, -1 until connected or listening
	bool nonblock;
	uint16_t bound_port;
	unsigned char reply[512];   // datagram (DNS) sockets: the answer waiting
	int reply_len;
};
Sock socks[MAX_SOCKS];

Sock *sock(int fd) {
	int i = fd - SOCK_BASE;
	return i >= 0 && i < MAX_SOCKS && socks[i].used ? &socks[i] : nullptr;
}

int new_socket(int type, bool nonblock, int k, int *fd) {
	for (int i = 0; i < MAX_SOCKS; i++)
		if (!socks[i].used) {
			socks[i] = {};
			socks[i].used = true;
			socks[i].type = type;
			socks[i].k = k;
			socks[i].nonblock = nonblock;
			*fd = SOCK_BASE + i;
			return 0;
		}
	return EMFILE;
}

// ---- Local (Unix-domain) sockets: the kernel's descriptors -------------------------

bool local(int fd) {
	if (fd < 0 || fd >= SOCK_BASE)
		return false;
	long fl = sc(TOS_FDINFO, fd);
	return fl >= 0 && (fl & TOS_SOCK_LOCAL);
}

bool knonblock[SOCK_BASE];                          // O_NONBLOCK of the kernel's descriptors

// Pipes and local sockets: the kernel answers EAGAIN instead of waiting.
int set_nonblock(int fd, bool on) {
	long r = sc(TOS_USOCK, TOS_USOCK_NONBLOCK, fd, on);
	if (r < 0 && r != TOS_ERR_BADFD)
		return to_errno(r);
	knonblock[fd] = on;                             // (files never wait anyway)
	return 0;
}

int local_name(const struct sockaddr *addr, socklen_t len, char *out) {
	size_t base = offsetof(sockaddr_un, sun_path);
	if (!addr || len <= base || addr->sa_family != AF_UNIX)
		return EINVAL;
	const sockaddr_un *un = (const sockaddr_un *)addr;
	size_t n = len - base < sizeof(un->sun_path) ? len - base : sizeof(un->sun_path);
	if (!un->sun_path[0])
		return EINVAL;                              // (no abstract names)
	memcpy(out, un->sun_path, n);
	out[n] = 0;
	return 0;
}

void fill_local(const char *name, struct sockaddr *addr, socklen_t *len) {
	if (!addr || !len)
		return;
	sockaddr_un un = {};
	un.sun_family = AF_UNIX;
	strncpy(un.sun_path, name, sizeof(un.sun_path) - 1);
	socklen_t full = offsetof(sockaddr_un, sun_path) + strlen(un.sun_path) + 1;
	memcpy(addr, &un, *len < full ? *len : full);
	*len = full;
}

int local_io(int fd, bool write, void *buf, size_t len, int flags, ssize_t *done) {
	if ((flags & MSG_DONTWAIT) && !knonblock[fd] &&
	    !(sc(TOS_FDINFO, fd) & (write ? TOS_SOCK_WRITABLE : TOS_SOCK_READABLE)))
		return EAGAIN;
	long r = sc(write ? TOS_WRITE : TOS_READ, fd, (long)buf, (long)len);
	if (r < 0)
		return to_errno(r);
	*done = r;
	return 0;
}

int sockinfo(Sock *s, tos_sockinfo *si) {
	long r = sc(TOS_SOCKINFO, s->k, (long)si);
	return r < 0 ? 0 : (int)r;
}

// A DNS question, answered from the kernel's resolver: A records only (an
// AAAA question gets an empty answer).
void answer_dns(Sock *s, const unsigned char *q, size_t len) {
	s->reply_len = 0;
	if (len < 17 || len > sizeof(s->reply) - 16)
		return;
	char name[256];
	size_t at = 12, n = 0;
	while (at < len && q[at]) {
		size_t l = q[at++];
		if (at + l > len || n + l + 1 >= sizeof(name))
			return;
		if (n)
			name[n++] = '.';
		memcpy(name + n, q + at, l);
		n += l;
		at += l;
	}
	name[n] = 0;
	if (at + 5 > len)
		return;
	size_t qend = at + 5;                           // the 0, type, class
	uint16_t qtype = (uint16_t)(q[at + 1] << 8 | q[at + 2]);
	unsigned char ip[4];
	bool found = qtype == 1 && sc(TOS_RESOLVE, (long)name, (long)ip) == 0;
	unsigned char *r = s->reply;
	memcpy(r, q, qend);
	r[2] = 0x81;                                    // a response, recursion desired
	r[3] = (qtype == 1 && !found) ? 0x83 : 0x80;    // recursion available; NXDOMAIN if not found
	r[4] = 0; r[5] = 1;                             // one question
	r[6] = 0; r[7] = found ? 1 : 0;                 // answers
	r[8] = r[9] = r[10] = r[11] = 0;
	size_t o = qend;
	if (found) {
		const unsigned char rr[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4 };
		memcpy(r + o, rr, sizeof(rr));
		o += sizeof(rr);
		memcpy(r + o, ip, 4);
		o += 4;
	}
	s->reply_len = (int)o;
}

int sock_send(Sock *s, const void *buf, size_t len, const struct sockaddr *to, ssize_t *done) {
	if (s->type == SOCK_DGRAM) {
		uint16_t port = 53;
		if (to)
			port = be16(((const sockaddr_in *)to)->sin_port);
		if (port != 53)
			return ENETUNREACH;                     // (no UDP but DNS)
		answer_dns(s, (const unsigned char *)buf, len);
		*done = (ssize_t)len;
		return 0;
	}
	if (s->k < 0)
		return ENOTCONN;
	long r = sc(TOS_SEND, s->k, (long)buf, (long)len);
	if (r < 0)
		return to_errno(r);
	*done = r;
	return 0;
}

int sock_recv(Sock *s, void *buf, size_t len, int flags, ssize_t *done) {
	if (s->type == SOCK_DGRAM) {
		if (!s->reply_len)
			return EAGAIN;
		size_t n = (size_t)s->reply_len < len ? (size_t)s->reply_len : len;
		memcpy(buf, s->reply, n);
		if (!(flags & MSG_PEEK))
			s->reply_len = 0;
		*done = (ssize_t)n;
		return 0;
	}
	if (s->k < 0)
		return ENOTCONN;
	if (flags & MSG_PEEK)
		return EOPNOTSUPP;
	bool nonblock = s->nonblock || (flags & MSG_DONTWAIT);
	if (nonblock) {
		tos_sockinfo si;
		int fl = sockinfo(s, &si);
		if (!(fl & TOS_SOCK_READABLE))
			return EAGAIN;
	}
	for (;;) {
		long r = sc(TOS_RECV, s->k, (long)buf, (long)len, nonblock ? 1 : 0);
		if (r == TOS_ERR_TIMEOUT) {
			if (nonblock)
				return EAGAIN;
			continue;
		}
		if (r < 0)
			return to_errno(r);
		*done = r;
		return 0;
	}
}

// ---- Built-in files ------------------------------------------------------------------

enum { VF_NULL, VF_ZERO, VF_RANDOM, VF_TEXT };

struct VFile {
	bool used;
	int kind;
	const char *text;
	size_t len, pos;
};
VFile vfiles[MAX_VFILES];

const char services[] =
	"ftp 21/tcp\nssh 22/tcp\ntelnet 23/tcp\nsmtp 25/tcp\ndomain 53/tcp\ndomain 53/udp\n"
	"http 80/tcp www\npop3 110/tcp\nimap 143/tcp\nhttps 443/tcp\nsubmission 587/tcp\n"
	"imaps 993/tcp\npop3s 995/tcp\nhttp-alt 8080/tcp\n";
const char hosts[] = "127.0.0.1 localhost\n";
const char resolv[] = "nameserver 127.0.0.53\n";   // (answered by the kernel's resolver)

struct Builtin {
	const char *path;
	int kind;
	const char *text;
	size_t len;
};
const Builtin builtins[] = {
	{"/dev/null", VF_NULL, nullptr, 0},
	{"/dev/zero", VF_ZERO, nullptr, 0},
	{"/dev/urandom", VF_RANDOM, nullptr, 0},
	{"/dev/random", VF_RANDOM, nullptr, 0},
	{"/etc/services", VF_TEXT, services, sizeof(services) - 1},
	{"/etc/hosts", VF_TEXT, hosts, sizeof(hosts) - 1},
	{"/etc/resolv.conf", VF_TEXT, resolv, sizeof(resolv) - 1},
};

const Builtin *find_builtin(const char *path) {
	for (const Builtin &b : builtins)
		if (!strcmp(path, b.path))
			return &b;
	return nullptr;
}

VFile *vfile(int fd) {
	int i = fd - VF_BASE;
	return i >= 0 && i < MAX_VFILES && vfiles[i].used ? &vfiles[i] : nullptr;
}

long now_ms() { return sc(TOS_UPTIME); }

// Readiness of one descriptor for poll().
short ready(int fd, short events) {
	short r = 0;
	if (Sock *s = sock(fd)) {
		if (s->type == SOCK_DGRAM) {
			r = POLLOUT | (s->reply_len ? POLLIN : 0);
		} else if (s->k < 0) {
			r = POLLOUT | POLLHUP;
		} else {
			tos_sockinfo si;
			int fl = sockinfo(s, &si);
			if (fl & TOS_SOCK_READABLE) r |= POLLIN;
			if (fl & TOS_SOCK_WRITABLE) r |= POLLOUT;
			if (fl & TOS_SOCK_HANGUP) r |= POLLHUP;
			if (fl & TOS_SOCK_ERROR) r |= POLLERR;
		}
	} else if (vfile(fd) || fd >= DIR_BASE) {
		r = POLLIN | POLLOUT;
	} else if (fd >= 0) {                           // the kernel's: files, pipes, the console
		long fl = sc(TOS_FDINFO, fd);
		if (fl < 0)
			return POLLNVAL;
		if (fl & TOS_SOCK_READABLE) r |= POLLIN;
		if (fl & TOS_SOCK_WRITABLE) r |= POLLOUT;
		if (fl & TOS_SOCK_HANGUP) r |= POLLHUP;
	}
	return (short)(r & (events | POLLHUP | POLLERR | POLLNVAL));
}

int poll_ms(struct pollfd *fds, nfds_t count, long timeout, int *num) {
	long start = now_ms();
	bool net = false;                               // TCP: packets come without waking anyone
	for (nfds_t i = 0; i < count; i++)
		if (Sock *s = sock(fds[i].fd); s && s->type == SOCK_STREAM)
			net = true;
	for (;;) {
		long gen = sc(TOS_POLLWAIT, -1);
		int n = 0;
		for (nfds_t i = 0; i < count; i++) {
			fds[i].revents = fds[i].fd < 0 ? 0 : ready(fds[i].fd, fds[i].events);
			if (fds[i].revents)
				n++;
		}
		if (n || timeout == 0 || (timeout > 0 && now_ms() - start >= timeout)) {
			*num = n;
			return 0;
		}
		long left = timeout < 0 ? 0x7FFFFFF : timeout - (now_ms() - start);
		sc(TOS_POLLWAIT, gen, left > 0 ? left : 1, net);   // until a pipe or local socket changes
	}
}

} // namespace

bool is_socket(int fd) { return sock(fd) != nullptr; }
bool is_vfile(int fd) { return vfile(fd) != nullptr; }

int special_read(int fd, void *buf, size_t count, ssize_t *done) {
	if (Sock *s = sock(fd))
		return sock_recv(s, buf, count, 0, done);
	VFile *v = vfile(fd);
	if (!v)
		return EBADF;
	switch (v->kind) {
	case VF_NULL:
		*done = 0;
		return 0;
	case VF_ZERO:
		memset(buf, 0, count);
		*done = (ssize_t)count;
		return 0;
	case VF_RANDOM:
		mlibc::sysdep<GetEntropy>(buf, count);
		*done = (ssize_t)count;
		return 0;
	default: {
		size_t n = v->pos < v->len ? v->len - v->pos : 0;
		if (n > count)
			n = count;
		memcpy(buf, v->text + v->pos, n);
		v->pos += n;
		*done = (ssize_t)n;
		return 0;
	}
	}
}

int special_write(int fd, const void *buf, size_t count, ssize_t *done) {
	if (Sock *s = sock(fd))
		return sock_send(s, buf, count, nullptr, done);
	VFile *v = vfile(fd);
	if (!v)
		return EBADF;
	if (v->kind == VF_TEXT)
		return EBADF;
	*done = (ssize_t)count;                         // /dev/null and friends swallow it
	return 0;
}

int special_close(int fd) {
	if (Sock *s = sock(fd)) {
		if (s->k >= 0)
			sc(TOS_SOCKCLOSE, s->k);
		*s = {};
		return 0;
	}
	if (VFile *v = vfile(fd)) {
		*v = {};
		return 0;
	}
	return EBADF;
}

int special_seek(int fd, off_t offset, int whence, off_t *pos) {
	VFile *v = vfile(fd);
	if (!v || v->kind != VF_TEXT)
		return ESPIPE;
	off_t base = whence == SEEK_SET ? 0 : whence == SEEK_CUR ? (off_t)v->pos : (off_t)v->len;
	if (base + offset < 0)
		return EINVAL;
	v->pos = (size_t)(base + offset);
	*pos = (off_t)v->pos;
	return 0;
}

int special_stat(int fd, struct stat *st) {
	memset(st, 0, sizeof(*st));
	st->st_nlink = 1;
	st->st_blksize = 4096;
	if (sock(fd)) {
		st->st_mode = S_IFSOCK | 0666;
		return 0;
	}
	if (VFile *v = vfile(fd)) {
		st->st_mode = v->kind == VF_TEXT ? (S_IFREG | 0444) : (S_IFCHR | 0666);
		st->st_size = v->kind == VF_TEXT ? (off_t)v->len : 0;
		return 0;
	}
	return EBADF;
}

int open_builtin(const char *path, int flags, int *fd, int *err) {
	const Builtin *b = find_builtin(path);
	if (!b)
		return 0;
	if (b->kind == VF_TEXT && (flags & O_ACCMODE) != O_RDONLY) {
		*err = EACCES;
		return 1;
	}
	for (int i = 0; i < MAX_VFILES; i++)
		if (!vfiles[i].used) {
			vfiles[i] = {true, b->kind, b->text, b->len, 0};
			*fd = VF_BASE + i;
			*err = 0;
			return 1;
		}
	*err = EMFILE;
	return 1;
}

bool builtin_exists(const char *path, struct stat *st) {
	const Builtin *b = find_builtin(path);
	if (!b)
		return false;
	if (st) {
		memset(st, 0, sizeof(*st));
		st->st_nlink = 1;
		st->st_blksize = 4096;
		st->st_mode = b->kind == VF_TEXT ? (S_IFREG | 0444) : (S_IFCHR | 0666);
		st->st_size = (off_t)b->len;
	}
	return true;
}

// The CA certificate bundle is CACERT.PEM in the system folder (the RAM disk,
// at / or /SYS); TLS libraries look for it under these names.
const char *redirect(const char *path) {
	static const char *const names[] = {
		"/etc/ssl/cert.pem", "/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt",
		"/etc/ssl/ca-bundle.pem", "/usr/share/ssl/cert.pem",
	};
	for (const char *n : names)
		if (!strcmp(path, n)) {
			tos_stat ks;
			if (sc(TOS_STAT, (long)"/SYS/CACERT.PEM", (long)&ks) == 0)
				return "/SYS/CACERT.PEM";
			return "/CACERT.PEM";
		}
	return path;
}

} // namespace tos

using namespace tos;

namespace mlibc {

int Sysdeps<Socket>::operator()(int family, int type, int, int *fd) {
	bool nonblock = type & SOCK_NONBLOCK, cloexec = type & SOCK_CLOEXEC;
	type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (family == AF_UNIX) {
		if (type != SOCK_STREAM)
			return EPROTONOSUPPORT;
		long r = sc(TOS_USOCK, TOS_USOCK_NEW);
		if (r < 0)
			return to_errno(r);
		*fd = (int)r;
		set_cloexec(*fd, cloexec);
		return set_nonblock(*fd, nonblock);
	}
	if (family != AF_INET)
		return EAFNOSUPPORT;
	if (type != SOCK_STREAM && type != SOCK_DGRAM)
		return EPROTONOSUPPORT;
	return new_socket(type, nonblock, -1, fd);
}

int Sysdeps<Socketpair>::operator()(int domain, int type, int, int *fds) {
	bool nonblock = type & SOCK_NONBLOCK, cloexec = type & SOCK_CLOEXEC;
	type &= ~(SOCK_NONBLOCK | SOCK_CLOEXEC);
	if (domain != AF_UNIX)
		return EAFNOSUPPORT;
	if (type != SOCK_STREAM)
		return EPROTONOSUPPORT;
	int pair[2];
	long r = sc(TOS_USOCK, TOS_USOCK_PAIR, (long)pair);
	if (r < 0)
		return to_errno(r);
	fds[0] = pair[0];
	fds[1] = pair[1];
	set_cloexec(pair[0], cloexec);
	set_cloexec(pair[1], cloexec);
	set_nonblock(pair[0], nonblock);
	return set_nonblock(pair[1], nonblock);
}

// Local sockets: bind, listen, connect and accept by name.
static int local_call(int op, int fd, const struct sockaddr *addr, socklen_t len) {
	char name[sizeof(sockaddr_un::sun_path) + 1];
	if (addr) {
		if (int e = local_name(addr, len, name))
			return e;
	}
	long r = sc(TOS_USOCK, op, fd, addr ? (long)name : 0);
	return r < 0 ? to_errno(r) : 0;
}

int Sysdeps<Connect>::operator()(int fd, const struct sockaddr *addr, socklen_t len) {
	if (local(fd))
		return local_call(TOS_USOCK_CONNECT, fd, addr, len);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	if (len < sizeof(sockaddr_in) || addr->sa_family != AF_INET)
		return EAFNOSUPPORT;
	if (s->type == SOCK_DGRAM)
		return 0;                                   // (the destination comes with each sendto)
	if (s->k >= 0)
		return EISCONN;
	const sockaddr_in *in = (const sockaddr_in *)addr;
	unsigned long ip = __builtin_bswap32(in->sin_addr.s_addr);
	// The kernel connects before returning, even for a non-blocking socket
	// (connect may finish at once; poll then says it is writable).
	long r = sc(TOS_CONNECT, (long)ip, be16(in->sin_port), 15000);
	if (r < 0)
		return to_errno(r);
	s->k = (int)r;
	return 0;
}

int Sysdeps<Bind>::operator()(int fd, const struct sockaddr *addr, socklen_t len) {
	if (local(fd))
		return local_call(TOS_USOCK_BIND, fd, addr, len);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	if (len < sizeof(sockaddr_in))
		return EINVAL;
	s->bound_port = be16(((const sockaddr_in *)addr)->sin_port);
	return 0;
}

int Sysdeps<Listen>::operator()(int fd, int) {
	if (local(fd))
		return local_call(TOS_USOCK_LISTEN, fd, nullptr, 0);
	Sock *s = sock(fd);
	if (!s || s->type != SOCK_STREAM)
		return ENOTSOCK;
	if (!s->bound_port)
		return EDESTADDRREQ;
	long r = sc(TOS_LISTEN, s->bound_port);
	if (r < 0)
		return to_errno(r);
	s->k = (int)r;
	return 0;
}

static void fill_addr(const unsigned char ip[4], uint16_t port, struct sockaddr *addr, socklen_t *len) {
	if (!addr || !len)
		return;
	sockaddr_in in = {};
	in.sin_family = AF_INET;
	in.sin_port = be16(port);
	memcpy(&in.sin_addr, ip, 4);
	socklen_t n = *len < sizeof(in) ? *len : sizeof(in);
	memcpy(addr, &in, n);
	*len = sizeof(in);
}

int Sysdeps<Accept>::operator()(int fd, int *newfd, struct sockaddr *addr, socklen_t *len, int flags) {
	if (local(fd)) {
		long r = sc(TOS_USOCK, TOS_USOCK_ACCEPT, fd);
		if (r < 0)
			return to_errno(r);
		*newfd = (int)r;
		set_cloexec(*newfd, flags & SOCK_CLOEXEC);
		fill_local("", addr, len);
		return set_nonblock(*newfd, flags & SOCK_NONBLOCK);
	}
	Sock *s = sock(fd);
	if (!s || s->k < 0)
		return EINVAL;
	long r = sc(TOS_ACCEPT, s->k, s->nonblock ? 1 : 0);
	if (r == TOS_ERR_TIMEOUT)
		return EAGAIN;
	if (r < 0)
		return to_errno(r);
	if (int e = new_socket(SOCK_STREAM, flags & SOCK_NONBLOCK, (int)r, newfd); e) {
		sc(TOS_SOCKCLOSE, r);
		return e;
	}
	tos_sockinfo si;
	sockinfo(sock(*newfd), &si);
	fill_addr(si.rip, si.rport, addr, len);
	return 0;
}

int Sysdeps<Sendto>::operator()(int fd, const void *buf, size_t size, int, const struct sockaddr *to,
                                socklen_t, ssize_t *length) {
	if (local(fd))
		return local_io(fd, true, (void *)buf, size, 0, length);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	return sock_send(s, buf, size, to, length);
}

int Sysdeps<Recvfrom>::operator()(int fd, void *buf, size_t size, int flags, struct sockaddr *from,
                                  socklen_t *fromlen, ssize_t *length) {
	if (local(fd)) {
		fill_local("", from, fromlen);
		return local_io(fd, false, buf, size, flags, length);
	}
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	int e = sock_recv(s, buf, size, flags, length);
	if (!e && from && fromlen) {
		if (s->type == SOCK_DGRAM) {
			const unsigned char ns[4] = {127, 0, 0, 53};
			fill_addr(ns, 53, from, fromlen);
		} else {
			tos_sockinfo si;
			sockinfo(s, &si);
			fill_addr(si.rip, si.rport, from, fromlen);
		}
	}
	return e;
}

// Local sockets: SCM_RIGHTS files go first, so they are there when the data is.
static int local_sendmsg(int fd, const struct msghdr *hdr, int flags, ssize_t *length) {
	bool has_fds = false;
	for (cmsghdr *c = CMSG_FIRSTHDR(hdr); c; c = CMSG_NXTHDR((msghdr *)hdr, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			has_fds = true;
	if (has_fds && (knonblock[fd] || (flags & MSG_DONTWAIT)) && !(sc(TOS_FDINFO, fd) & TOS_SOCK_WRITABLE))
		return EAGAIN;                              // (not the files without the data)
	for (cmsghdr *c = CMSG_FIRSTHDR(hdr); c; c = CMSG_NXTHDR((msghdr *)hdr, c))
		if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
			long n = (long)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
			long r = sc(TOS_USOCK, TOS_USOCK_SENDFDS, fd, (long)CMSG_DATA(c), n);
			if (r < 0)
				return to_errno(r);
		}
	ssize_t total = 0;
	for (int i = 0; i < (int)hdr->msg_iovlen; i++) {
		ssize_t n = 0;
		size_t want = hdr->msg_iov[i].iov_len;
		if (!want)
			continue;
		int e = local_io(fd, true, hdr->msg_iov[i].iov_base, want, flags, &n);
		if (e)
			return total ? (*length = total, 0) : e;
		total += n;
		if ((size_t)n < want)
			break;
	}
	*length = total;
	return 0;
}

static int local_recvmsg(int fd, struct msghdr *hdr, int flags, ssize_t *length) {
	*length = 0;
	hdr->msg_flags = 0;
	for (int i = 0; i < (int)hdr->msg_iovlen; i++)          // (into the first buffer with room)
		if (hdr->msg_iov[i].iov_len) {
			if (int e = local_io(fd, false, hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len, flags, length))
				return e;
			break;
		}
	size_t room = hdr->msg_control && hdr->msg_controllen > CMSG_LEN(0)
	                  ? (hdr->msg_controllen - CMSG_LEN(0)) / sizeof(int) : 0;
	if (room > TOS_USOCK_MAX_FDS)
		room = TOS_USOCK_MAX_FDS;
	long n = room ? sc(TOS_USOCK, TOS_USOCK_RECVFDS, fd, (long)CMSG_DATA((cmsghdr *)hdr->msg_control), (long)room) : 0;
	if (n > 0) {
		cmsghdr *c = (cmsghdr *)hdr->msg_control;
		c->cmsg_level = SOL_SOCKET;
		c->cmsg_type = SCM_RIGHTS;
		c->cmsg_len = CMSG_LEN(n * sizeof(int));
		hdr->msg_controllen = CMSG_SPACE(n * sizeof(int));
		for (long i = 0; i < n; i++)
			knonblock[((int *)CMSG_DATA(c))[i]] = false;
	} else {
		hdr->msg_controllen = 0;
	}
	if (hdr->msg_name && hdr->msg_namelen)
		fill_local("", (struct sockaddr *)hdr->msg_name, &hdr->msg_namelen);
	return 0;
}

int Sysdeps<MsgSend>::operator()(int fd, const struct msghdr *hdr, int flags, ssize_t *length) {
	if (local(fd))
		return local_sendmsg(fd, hdr, flags, length);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	ssize_t total = 0;
	for (int i = 0; i < (int)hdr->msg_iovlen; i++) {
		ssize_t n = 0;
		if (!hdr->msg_iov[i].iov_len)
			continue;
		int e = sock_send(s, hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len,
		                  (const struct sockaddr *)hdr->msg_name, &n);
		if (e)
			return total ? (*length = total, 0) : e;
		total += n;
	}
	(void)flags;
	*length = total;
	return 0;
}

int Sysdeps<MsgRecv>::operator()(int fd, struct msghdr *hdr, int flags, ssize_t *length) {
	if (local(fd))
		return local_recvmsg(fd, hdr, flags, length);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	for (int i = 0; i < (int)hdr->msg_iovlen; i++)          // (into the first buffer with room)
		if (hdr->msg_iov[i].iov_len) {
			hdr->msg_controllen = 0;
			hdr->msg_flags = 0;
			return sock_recv(s, hdr->msg_iov[i].iov_base, hdr->msg_iov[i].iov_len, flags, length);
		}
	*length = 0;
	return 0;
}

int Sysdeps<Shutdown>::operator()(int fd, int) {
	return sock(fd) || local(fd) ? 0 : ENOTSOCK;    // (no half-close: the data still flows)
}

int Sysdeps<SetSockopt>::operator()(int fd, int, int, const void *, socklen_t) {
	return sock(fd) || local(fd) ? 0 : ENOTSOCK;    // accepted and ignored
}

int Sysdeps<GetSockopt>::operator()(int fd, int layer, int number, void *__restrict buffer,
                                    socklen_t *__restrict size) {
	Sock *s = sock(fd);
	if (!s && !local(fd))
		return ENOTSOCK;
	int value = 0;
	if (!s) {
		if (layer == SOL_SOCKET && number == SO_TYPE)
			value = SOCK_STREAM;
		else if (layer == SOL_SOCKET && (number == SO_RCVBUF || number == SO_SNDBUF))
			value = 16384;
	} else if (layer == SOL_SOCKET && number == SO_ERROR && s->k >= 0 && s->type == SOCK_STREAM) {
		tos_sockinfo si;
		if (sockinfo(s, &si) & TOS_SOCK_ERROR)
			value = ECONNRESET;
	} else if (layer == SOL_SOCKET && number == SO_TYPE) {
		value = s->type;
	} else if (layer == SOL_SOCKET && (number == SO_RCVBUF || number == SO_SNDBUF)) {
		value = 65536;
	}
	if (*size >= sizeof(int))
		*(int *)buffer = value;
	*size = sizeof(int);
	return 0;
}

static int local_sockname(int fd, struct sockaddr *addr, socklen_t max, socklen_t *actual) {
	char name[108];
	long r = sc(TOS_USOCK, TOS_USOCK_NAME, fd, (long)name);
	if (r < 0)
		return to_errno(r);
	socklen_t len = max;
	fill_local(name, addr, &len);
	*actual = len;
	return 0;
}

int Sysdeps<Sockname>::operator()(int fd, struct sockaddr *addr, socklen_t max, socklen_t *actual) {
	if (local(fd))
		return local_sockname(fd, addr, max, actual);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	uint16_t port = s->bound_port;
	if (s->k >= 0) {
		tos_sockinfo si;
		sockinfo(s, &si);
		port = si.lport;
	}
	const unsigned char any[4] = {0, 0, 0, 0};
	socklen_t len = max;
	fill_addr(any, port, addr, &len);
	*actual = len;
	return 0;
}

int Sysdeps<Peername>::operator()(int fd, struct sockaddr *addr, socklen_t max, socklen_t *actual) {
	if (local(fd))
		return local_sockname(fd, addr, max, actual);
	Sock *s = sock(fd);
	if (!s)
		return ENOTSOCK;
	if (s->k < 0 || s->type != SOCK_STREAM)
		return ENOTCONN;
	tos_sockinfo si;
	sockinfo(s, &si);
	socklen_t len = max;
	fill_addr(si.rip, si.rport, addr, &len);
	*actual = len;
	return 0;
}

int Sysdeps<InetConfigured>::operator()(bool *ipv4, bool *ipv6) {
	*ipv4 = true;
	*ipv6 = false;
	return 0;
}

int Sysdeps<Poll>::operator()(struct pollfd *fds, nfds_t count, int timeout, int *num_events) {
	return poll_ms(fds, count, timeout, num_events);
}

int Sysdeps<Ppoll>::operator()(struct pollfd *fds, nfds_t count, const struct timespec *ts, const sigset_t *,
                               int *num_events) {
	long ms = ts ? ts->tv_sec * 1000 + ts->tv_nsec / 1000000 : -1;
	return poll_ms(fds, count, ms, num_events);
}

int Sysdeps<Pselect>::operator()(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, const struct timespec *timeout,
                                 const sigset_t *, int *num_events) {
	struct pollfd list[FD_SETSIZE];
	int n = 0;
	for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
		short ev = 0;
		if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
		if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
		if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
		if (ev)
			list[n++] = {fd, ev, 0};
	}
	long ms = timeout ? timeout->tv_sec * 1000 + timeout->tv_nsec / 1000000 : -1;
	int ready_count;
	poll_ms(list, n, ms, &ready_count);
	if (rd) FD_ZERO(rd);
	if (wr) FD_ZERO(wr);
	if (ex) FD_ZERO(ex);
	int bits = 0;
	for (int i = 0; i < n; i++) {
		if (rd && (list[i].revents & (POLLIN | POLLHUP | POLLERR))) { FD_SET(list[i].fd, rd); bits++; }
		if (wr && (list[i].revents & (POLLOUT | POLLERR))) { FD_SET(list[i].fd, wr); bits++; }
	}
	*num_events = bits;
	return 0;
}

int Sysdeps<Fcntl>::operator()(int fd, int request, va_list args, int *result) {
	Sock *s = sock(fd);
	switch (request) {
	case F_GETFL:
		*result = O_RDWR | ((s ? s->nonblock : fd >= 0 && fd < SOCK_BASE && knonblock[fd]) ? O_NONBLOCK : 0);
		return 0;
	case F_SETFL: {
		int flags = va_arg(args, int);
		if (s)
			s->nonblock = flags & O_NONBLOCK;
		else if (fd >= 0 && fd < SOCK_BASE)
			if (int e = set_nonblock(fd, flags & O_NONBLOCK))
				return e;
		*result = 0;
		return 0;
	}
	case F_GETFD:
		*result = fd >= 0 && fd < SOCK_BASE && sc(TOS_FDFLAGS, fd, -1) == 1 ? FD_CLOEXEC : 0;
		return 0;
	case F_SETFD:
		set_cloexec(fd, va_arg(args, int) & FD_CLOEXEC);
		*result = 0;
		return 0;
	case F_DUPFD:
	case F_DUPFD_CLOEXEC: {
		if (s || is_vfile(fd) || fd >= DIR_BASE)
			return EINVAL;
		long r = sc(TOS_DUP, fd);
		if (r < 0)
			return to_errno(r);
		*result = (int)r;
		return 0;
	}
	case F_GETLK:
	case F_SETLK:
	case F_SETLKW:
		*result = 0;                                // (one program per file: locks always succeed)
		return 0;
	default:
		return EINVAL;
	}
}

int Sysdeps<Ioctl>::operator()(int fd, unsigned long request, void *arg, int *result) {
	Sock *s = sock(fd);
	*result = 0;
	if (request == FIONBIO) {
		if (!s && fd >= 0 && fd < SOCK_BASE)
			return set_nonblock(fd, *(int *)arg != 0);
		if (!s)
			return ENOTTY;
		s->nonblock = *(int *)arg != 0;
		return 0;
	}
	if (request == FIONREAD) {
		int n = 0;
		if (s && s->type == SOCK_DGRAM) {
			n = s->reply_len;
		} else if (s && s->k >= 0) {
			tos_sockinfo si;
			sockinfo(s, &si);
			n = (int)si.available;
		}
		*(int *)arg = n;
		return 0;
	}
	if (request == TIOCGWINSZ && fd >= 0 && fd <= 2) {
		struct winsize *w = (struct winsize *)arg;
		memset(w, 0, sizeof(*w));
		w->ws_row = 25;
		w->ws_col = 80;
		return 0;
	}
	return ENOTTY;
}

} // namespace mlibc
