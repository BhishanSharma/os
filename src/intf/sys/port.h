// port.h - Mach-style ports: message queues and the rights to use them
//
// A port is a queue of messages in the kernel. A program refers to ports by
// small numbers (names) in its own table, each with a right: RECEIVE (one
// holder: the port's owner), SEND, or SEND_ONCE (used up by one message,
// typically a reply). A message can carry a right to a reply port; the
// receiver gets a SEND_ONCE right to it. Services register a port under a
// name (port_register) and clients get a SEND right with port_lookup.
#ifndef PORT_H
#define PORT_H

#include <stdint.h>

#define PORT_RIGHTS     32
#define PORT_MSG_MAX    (1024 * 1024)
#define PORT_RIGHT_RECEIVE   1
#define PORT_RIGHT_SEND      2
#define PORT_RIGHT_SEND_ONCE 3

typedef struct { int port, type; } port_right_t;      /* port: index + 1, 0 = empty slot */
typedef struct { port_right_t r[PORT_RIGHTS]; } port_space_t;

/* Results >= 0, or SYSERR_* (sys/syscall_nums.h). */
int port_create(port_space_t *s);                      /* -> name with a RECEIVE right */
int port_send(port_space_t *s, int name, const void *msg, uint32_t len, int reply_name);
int port_receive(port_space_t *s, int name, void *buf, uint32_t max, uint32_t timeout_ms, int *reply_name);
int port_register(port_space_t *s, const char *service, int name);
int port_lookup(port_space_t *s, const char *service); /* -> name with a SEND right */
int port_close(port_space_t *s, int name);
void port_space_free(port_space_t *s);                 /* a program ended */

#endif
