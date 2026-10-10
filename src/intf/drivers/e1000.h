#ifndef E1000_H
#define E1000_H
#include <stdint.h>
#include "drivers/nic.h"

/* Intel PRO/1000 (e1000) gigabit Ethernet: the card VirtualBox emulates by
 * default (82540EM "PRO/1000 MT Desktop"), also QEMU's `-device e1000`, and
 * the 82545EM / 82543GC / 82574L variants. Descriptor-ring DMA through the
 * memory-mapped register window (BAR0). */

int  e1000_probe_init(void);          /* 0 on success */
uint8_t e1000_get_irq(void);          /* PIC line, or NIC_IRQ_NONE if unusable */
void e1000_handle_irq(void);          /* IRQ handler; also safe to call to poll */
void e1000_get_mac(uint8_t mac[6]);
void e1000_set_rx_handler(nic_rx_cb_t cb);
void e1000_get_stats(nic_stats_t *out);
int  e1000_send(const void *frame, uint16_t len);
int  e1000_link_up(void);             /* 1 if the link is up */
const char *e1000_model(void);        /* "82540EM", ... */

#endif
