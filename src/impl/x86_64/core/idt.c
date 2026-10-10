#include "core/idt.h"
#include "core/exceptions.h"
#include <stddef.h>

#define IDT_MAX 256

extern void idt_load(struct IDTDescriptor*); // defined in ASM

static struct IDTEntry idt[IDT_MAX];

void idt_set_entry(int vector, void* isr, uint8_t flags) {
    idt_set_entry_ist(vector, isr, flags, 0);
}

void idt_set_entry_ist(int vector, void* isr, uint8_t flags, uint8_t ist) {
    uint64_t addr = (uint64_t)isr;

    idt[vector].offset_low  = addr & 0xFFFF;
    idt[vector].selector    = 0x08;       // kernel code segment (from GDT)
    idt[vector].ist         = ist & 7;
    idt[vector].type_attr   = flags;
    idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero        = 0;
}

/* Another core: the same table. */
void idt_load_ap(void) {
    struct IDTDescriptor idtd;
    idtd.limit = sizeof(idt) - 1;
    idtd.base  = (uint64_t)&idt;
    idt_load(&idtd);
}

void idt_init() {
    struct IDTDescriptor idtd;
    idtd.limit = sizeof(idt) - 1;
    idtd.base  = (uint64_t)&idt;

    // CPU exceptions 0-31 get real handlers (register dump + panic screen)
    // before anything else can go wrong.
    exceptions_init();

    // load with lidt
    idt_load(&idtd);
}
