#ifndef RTL8139_H
#define RTL8139_H
#include <stdint.h>

int rtl8139_probe_init(void); // returns 0 on success
uint8_t rtl8139_get_irq(void); // PCI interrupt line; install the IDT entry at 0x20 + this
void rtl8139_handle_irq(void); // call from your IRQ stub or register an IDT entry

#endif
