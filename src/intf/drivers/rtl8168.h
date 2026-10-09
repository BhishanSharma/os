#ifndef RTL8168_H
#define RTL8168_H
#include <stdint.h>
#include "drivers/nic.h"

/* Realtek RTL8168/8111 (and RTL8169) gigabit Ethernet, the usual on-board NIC
 * of laptops and desktops. Descriptor-ring DMA through the I/O-port BAR. */

int  rtl8168_probe_init(void);         /* 0 on success */
uint8_t rtl8168_get_irq(void);         /* PIC line, or NIC_IRQ_NONE if unusable */
void rtl8168_handle_irq(void);         /* IRQ handler; also safe to call to poll */
void rtl8168_get_mac(uint8_t mac[6]);
void rtl8168_set_rx_handler(nic_rx_cb_t cb);
void rtl8168_get_stats(nic_stats_t *out);
int  rtl8168_send(const void *frame, uint16_t len);
int  rtl8168_link_up(void);            /* 1 if the PHY reports a link */

#endif
