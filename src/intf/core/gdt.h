#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Selectors in the kernel GDT.
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_TSS         0x18

// IST slot used for the fatal exceptions (#NMI, #DF, #MC) so they get a
// known-good stack even when the kernel stack is what broke.
#define IST_FATAL 1

// Build the GDT (null, code, data, TSS), load it, reload the segment
// registers and load the task register. Call before idt_init().
void gdt_init(void);

#endif
