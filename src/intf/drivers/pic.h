#ifndef PIC_H
#define PIC_H

#include <stdint.h>

// Remap the 8259 PICs to vectors 0x20-0x2F and mask everything except IRQ1.
void pic_remap(void);

// Unmask one IRQ line (0-15). IRQs 8-15 live on the slave PIC, so this also
// unmasks the cascade line (IRQ2) on the master.
void enable_irq(uint8_t irq);

#endif
