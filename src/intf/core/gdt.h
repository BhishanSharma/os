#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Selectors in the kernel GDT.
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_TSS         0x18
#define GDT_USER_CODE   0x28    // use with RPL 3: 0x2B
#define GDT_USER_DATA   0x30    // use with RPL 3: 0x33

// IST slot used for the fatal exceptions (#NMI, #DF, #MC) so they get a
// known-good stack even when the kernel stack is what broke.
#define IST_FATAL 1

// Build the GDT (null, code, data, TSS), load it, reload the segment
// registers and load the task register. Call before idt_init().
void gdt_init(void);

// Another core: load the same GDT and its own TSS.
void gdt_init_ap(int cpu, uint64_t fatal_stack_top);

// Stack this core switches to when an interrupt or system call arrives while a
// user program (ring 3) runs (its TSS.rsp0).
void gdt_set_kernel_stack(uint64_t top);

#endif
