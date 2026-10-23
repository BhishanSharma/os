// syscall_net.c - the TCP socket system calls (from sys/process.c)
//
// Sockets belong to the program that opened or accepted them (tcp_owner); a
// program can only use its own.
#include "process_internal.h"
#include "sys/task.h"
#include "sys/syscall_nums.h"
#include "net/tcp.h"
#include "net/net.h"
#include "lib/string.h"

static int own_socket(int s) { return tcp_owner(s) == task_current()->pid; }

/* A TCP result for the program: new sockets become its own; Ctrl+C ends it. */
static int64_t sock_result(int r, int is_new_socket) {
    if (r >= 0) {
        if (is_new_socket) tcp_set_owner(r, task_current()->pid);
        return r;
    }
    switch (r) {
        case TCP_ERR_INTR:    check_killed(); return SYSERR_TIMEOUT;
        case TCP_ERR_TIMEOUT: return SYSERR_TIMEOUT;
        case TCP_ERR_REFUSED: return SYSERR_REFUSED;
        case TCP_ERR_RESET:   return SYSERR_RESET;
        case TCP_ERR_CLOSED:  return SYSERR_CLOSED;
        case TCP_ERR_NOROUTE: return SYSERR_NOROUTE;
        case TCP_ERR_INUSE:   return SYSERR_INUSE;
        case TCP_ERR_NOSOCK:  return is_new_socket ? SYSERR_AGAIN : SYSERR_BADFD;
        default:              return SYSERR_IO;
    }
}

static int64_t sys_resolve(uint64_t host_ptr, uint64_t ip_ptr) {
    char host[128];
    if (copy_user_string(host, host_ptr, sizeof(host)) != 0 || !user_range_ok(ip_ptr, 4, 1)) return SYSERR_FAULT;
    if (!net_is_up()) return SYSERR_NOROUTE;
    uint8_t ip[4];
    if (net_resolve(host, ip) != 0) return SYSERR_NOHOST;
    memcpy((void *)ip_ptr, ip, 4);
    return 0;
}

static int64_t sys_connect(uint64_t packed, uint64_t port, uint64_t timeout_ms) {
    if (!net_is_up()) return SYSERR_NOROUTE;
    if (!port || port > 65535) return SYSERR_BADCALL;
    uint8_t ip[4] = { (uint8_t)(packed >> 24), (uint8_t)(packed >> 16), (uint8_t)(packed >> 8), (uint8_t)packed };
    return sock_result(tcp_connect(ip, (uint16_t)port, timeout_ms ? (uint32_t)timeout_ms : 10000), 1);
}

static int64_t sys_accept(uint64_t s, uint64_t timeout_ms) {
    if (!own_socket((int)s)) return SYSERR_BADFD;
    uint32_t ms = timeout_ms && timeout_ms < NO_LIMIT_MS ? (uint32_t)timeout_ms : NO_LIMIT_MS;
    return sock_result(tcp_accept((int)s, ms), 1);
}

static int64_t sys_send(uint64_t s, uint64_t buf, uint64_t len) {
    if (!own_socket((int)s)) return SYSERR_BADFD;
    if (!user_range_ok(buf, len, 0)) return SYSERR_FAULT;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    return sock_result(tcp_send((int)s, (const void *)buf, (uint32_t)len), 0);
}

static int64_t sys_recv(uint64_t s, uint64_t buf, uint64_t len, uint64_t timeout_ms) {
    if (!own_socket((int)s)) return SYSERR_BADFD;
    if (!user_range_ok(buf, len, 1)) return SYSERR_FAULT;
    if (len > 0x7FFFFFFF) len = 0x7FFFFFFF;
    uint32_t ms = timeout_ms && timeout_ms < NO_LIMIT_MS ? (uint32_t)timeout_ms : NO_LIMIT_MS;
    return sock_result(tcp_recv((int)s, (void *)buf, (uint32_t)len, ms), 0);
}

int64_t sys_net(uint64_t nr, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
    int64_t r;
    switch (nr) {
        case SYS_RESOLVE: return sys_resolve(a, b);
        case SYS_CONNECT: return sys_connect(a, b, c);
        case SYS_LISTEN:  return sock_result(tcp_listen((uint16_t)a), 1);
        case SYS_ACCEPT:  return sys_accept(a, b);
        case SYS_SEND:    return sys_send(a, b, c);
        case SYS_RECV:    return sys_recv(a, b, c, d);
        case SYS_SOCKINFO: {
            struct os_sockinfo *si = (struct os_sockinfo *)b;
            if (!own_socket((int)a)) { r = SYSERR_BADFD; break; }
            if (!user_range_ok(b, sizeof(*si), 1)) { r = SYSERR_FAULT; break; }
            uint32_t avail;
            uint16_t lp, rp;
            uint8_t ip[4];
            int fl = tcp_info((int)a, &avail, &lp, &rp, ip);
            if (fl < 0) { r = SYSERR_BADFD; break; }
            si->available = avail;
            si->lport = lp;
            si->rport = rp;
            memcpy(si->rip, ip, 4);
            r = fl;
            break;
        }
        case SYS_SOCKCLOSE: r = own_socket((int)a) ? (tcp_close((int)a), 0) : SYSERR_BADFD; break;
        default:          return SYSERR_BADCALL;
    }
    return r;
}
