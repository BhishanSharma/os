// net_internal.h - what net.c offers the transport code (net/tcp.c)
#ifndef NET_INTERNAL_H
#define NET_INTERNAL_H

#include <stdint.h>

#define IP_PROTO_TCP 6

const uint8_t *net_my_ip(void);
/* The Ethernet address to send to for `dst` (it, or the gateway); may wait
 * for ARP, so only from a waiting context. 0 on success. */
int net_next_hop_mac(const uint8_t dst[4], uint8_t mac[6]);
/* An IPv4 packet carrying `payload`. Safe from the receive path. */
int net_send_ip(const uint8_t mac[6], const uint8_t dst[4], uint8_t proto, const uint8_t *payload, uint16_t len);
uint16_t net_l4_checksum(const uint8_t src[4], const uint8_t dst[4], uint8_t proto, const uint8_t *seg, uint16_t len);

/* Waiting for the network (yield, then sleep until an interrupt), and
 * keeping the receive path out of a critical section. */
void net_wait(void);
uint64_t net_irq_save(void);
void net_irq_restore(uint64_t flags);

/* From the receive path: a TCP segment for us (ip = IPv4 header). */
void tcp_input(const uint8_t *frame, const uint8_t *ip, uint16_t ihl, uint16_t total);

#endif
