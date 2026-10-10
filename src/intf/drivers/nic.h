#ifndef NIC_H
#define NIC_H
#include <stdint.h>

/* Driver-independent network card interface. nic_probe_init() picks the first
 * supported card it finds (RTL8139 under QEMU, RTL8168/8111 on real PCs,
 * Intel e1000 under VirtualBox); the
 * network stack only talks to these functions. */

#define NIC_MAX_FRAME 1514   /* largest Ethernet frame without the 4-byte CRC */

/* Called for every good received frame (CRC stripped), in IRQ context. `frame`
 * is only valid during the call. Keep the handler short. */
typedef void (*nic_rx_cb_t)(const uint8_t *frame, uint16_t len);

typedef struct {
    uint32_t rx_packets, rx_bytes, rx_errors;
    uint32_t tx_packets, tx_bytes, tx_errors;
} nic_stats_t;

typedef struct {
    const char *name;
    uint8_t irq;                          /* PIC line, or NIC_IRQ_NONE to poll */
    void (*handle_irq)(void);
    void (*get_mac)(uint8_t mac[6]);
    void (*set_rx_handler)(nic_rx_cb_t cb);
    void (*get_stats)(nic_stats_t *out);
    int  (*send)(const void *frame, uint16_t len);
} nic_driver_t;

#define NIC_IRQ_NONE 0xFF

int  nic_probe_init(void);           /* 0 if a supported card was initialized */
int  nic_is_up(void);
const char *nic_name(void);
uint8_t nic_get_irq(void);           /* PIC line to install at 0x20 + irq, or NIC_IRQ_NONE */
void nic_handle_irq(void);           /* called from the NIC IRQ stub and when polling */
void nic_get_mac(uint8_t mac[6]);
void nic_set_rx_handler(nic_rx_cb_t cb);
void nic_get_stats(nic_stats_t *out);
int  nic_send(const void *frame, uint16_t len);   /* 0 on success */

#endif
