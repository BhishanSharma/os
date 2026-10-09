#ifndef RTL8139_H
#define RTL8139_H
#include <stdint.h>
#include "drivers/nic.h"

/* Largest Ethernet frame we send or receive (without the 4-byte CRC). */
#define RTL8139_MAX_FRAME NIC_MAX_FRAME

/* See nic_rx_cb_t: called in IRQ context for every good frame. */
typedef nic_rx_cb_t rtl8139_rx_cb_t;
typedef nic_stats_t rtl8139_stats_t;

int rtl8139_probe_init(void);   // returns 0 on success
uint8_t rtl8139_get_irq(void);  // PCI interrupt line; install the IDT entry at 0x20 + this
void rtl8139_handle_irq(void);  // call from your IRQ stub or register an IDT entry

int  rtl8139_is_up(void);                         // 1 once probe_init succeeded
void rtl8139_get_mac(uint8_t mac[6]);             // the card's MAC address
void rtl8139_set_rx_handler(rtl8139_rx_cb_t cb);  // where received frames go
void rtl8139_get_stats(rtl8139_stats_t *out);

/* Send one raw Ethernet frame (dst MAC + src MAC + type + payload, no CRC).
 * Frames shorter than 60 bytes are zero-padded. Returns 0 on success, -1 on
 * error (NIC not ready, bad length, or no free TX descriptor). Safe to call
 * from both normal and IRQ context. */
int rtl8139_send(const void *frame, uint16_t len);

#endif
