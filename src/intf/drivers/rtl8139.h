#ifndef RTL8139_H
#define RTL8139_H
#include <stdint.h>

int rtl8139_probe_init(void); // returns 0 on success
void rtl8139_handle_irq(void); // call from your IRQ stub or register an IDT entry

#endif
