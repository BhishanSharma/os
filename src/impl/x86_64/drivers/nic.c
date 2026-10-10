#include "drivers/nic.h"
#include "drivers/rtl8139.h"
#include "drivers/rtl8168.h"
#include "drivers/e1000.h"
#include "drivers/timer.h"
#include <stdint.h>

static nic_driver_t rtl8139_drv = {
    "RTL8139", NIC_IRQ_NONE, rtl8139_handle_irq, rtl8139_get_mac,
    rtl8139_set_rx_handler, rtl8139_get_stats, rtl8139_send,
};

static nic_driver_t rtl8168_drv = {
    "RTL8168", NIC_IRQ_NONE, rtl8168_handle_irq, rtl8168_get_mac,
    rtl8168_set_rx_handler, rtl8168_get_stats, rtl8168_send,
};

static nic_driver_t e1000_drv = {
    "Intel e1000", NIC_IRQ_NONE, e1000_handle_irq, e1000_get_mac,
    e1000_set_rx_handler, e1000_get_stats, e1000_send,
};

static nic_driver_t *active;

int nic_probe_init(void) {
    if (rtl8139_probe_init() == 0) {
        rtl8139_drv.irq = rtl8139_get_irq();
        active = &rtl8139_drv;
        return 0;
    }
    if (rtl8168_probe_init() == 0) {
        rtl8168_drv.irq = rtl8168_get_irq();
        active = &rtl8168_drv;
        /* On UEFI machines the legacy PIC line is often unrouted, so poll the
         * receive ring from the timer as well. Handling is idempotent. */
        timer_set_poll_hook(nic_handle_irq);
        return 0;
    }
    if (e1000_probe_init() == 0) {          /* VirtualBox, QEMU -device e1000 */
        e1000_drv.irq = e1000_get_irq();
        active = &e1000_drv;
        timer_set_poll_hook(nic_handle_irq);
        return 0;
    }
    return -1;
}

int nic_is_up(void) { return active != 0; }

const char *nic_name(void) { return active ? active->name : "none"; }

uint8_t nic_get_irq(void) { return active ? active->irq : NIC_IRQ_NONE; }

void nic_handle_irq(void) { if (active) active->handle_irq(); }

void nic_get_mac(uint8_t mac[6]) { if (active) active->get_mac(mac); }

void nic_set_rx_handler(nic_rx_cb_t cb) { if (active) active->set_rx_handler(cb); }

void nic_get_stats(nic_stats_t *out) {
    if (active) { active->get_stats(out); return; }
    *out = (nic_stats_t){0};
}

int nic_send(const void *frame, uint16_t len) { return active ? active->send(frame, len) : -1; }
