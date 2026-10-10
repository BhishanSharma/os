// tcp.h - TCP connections (RFC 793/9293, the parts a client and a small server need)
//
// Up to TCP_MAX_SOCKETS connections at once, each with a 64 KiB receive ring
// and a 64 KiB send buffer. Data we send is kept until the peer acknowledges
// it and resent on a timeout (with backoff); the peer's window is respected,
// and ours follows the free space in the receive ring. Segments that arrive
// out of order are dropped and re-acknowledged (the peer resends them).
// Listening sockets accept connections into a small queue.
#ifndef TCP_H
#define TCP_H

#include <stdint.h>

#define TCP_MAX_SOCKETS 16

#define TCP_ERR_NOSOCK     (-1)    /* no free socket / bad socket number */
#define TCP_ERR_TIMEOUT    (-2)
#define TCP_ERR_REFUSED    (-3)    /* the peer answered with a reset */
#define TCP_ERR_RESET      (-4)    /* the connection was reset */
#define TCP_ERR_CLOSED     (-5)    /* sending on a closed connection */
#define TCP_ERR_NOROUTE    (-6)    /* no ARP answer for the next hop */
#define TCP_ERR_INUSE      (-7)    /* port already listening */
#define TCP_ERR_INTR       (-8)    /* Ctrl+C */

/* A connection to ip:port; waits up to `timeout_ms`. Returns a socket number. */
int tcp_connect(const uint8_t ip[4], uint16_t port, uint32_t timeout_ms);
/* Accept connections on `port`. Returns a listening socket number. */
int tcp_listen(uint16_t port);
/* The next connection on a listening socket (0 ms = do not wait). */
int tcp_accept(int listener, uint32_t timeout_ms);

/* Queue `len` bytes (waits while the send buffer is full). Returns len or an error. */
int tcp_send(int s, const void *buf, uint32_t len);
/* Up to `len` received bytes; waits up to `timeout_ms` for some.
 * > 0 bytes, 0 = the peer closed (end of data), < 0 error (TCP_ERR_TIMEOUT). */
int tcp_recv(int s, void *buf, uint32_t len, uint32_t timeout_ms);
/* Bytes waiting to be read. */
uint32_t tcp_available(int s);

/* Close (our data is still delivered, then FIN). The socket number is free
 * to reuse at once; the connection finishes in the background. */
int tcp_close(int s);

/* The process that owns new sockets (0 = the kernel); its sockets are
 * closed when it exits. */
void tcp_set_owner(int s, int pid);
int tcp_owner(int s);                  /* -1 if `s` is not an open socket */
void tcp_close_owned_by(int pid);

/* Waiting calls give up (TCP_ERR_INTR) when `check` says so (Ctrl+C, kill). */
void tcp_set_interrupt_check(int (*check)(void));

/* `netstat`. */
void tcp_print_sockets(void);

/* Every timer tick: resends, keep the TIME-WAIT clock. */
void tcp_tick(void);

#endif
