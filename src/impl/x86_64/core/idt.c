#include "idt.h"
#include <stddef.h>

#define IDT_MAX 256

extern void idt_load(struct IDTDescriptor*); // defined in ASM

static struct IDTEntry idt[IDT_MAX];

void idt_set_entry(int vector, void* isr, uint8_t flags) {
    uint64_t addr = (uint64_t)isr;

    idt[vector].offset_low  = addr & 0xFFFF;
    idt[vector].selector    = 0x08;       // kernel code segment (from GDT)
    idt[vector].ist         = 0;
    idt[vector].type_attr   = flags;
    idt[vector].offset_mid  = (addr >> 16) & 0xFFFF;
    idt[vector].offset_high = (addr >> 32) & 0xFFFFFFFF;
    idt[vector].zero        = 0;
}

void idt_init() {
    struct IDTDescriptor idtd;
    idtd.limit = sizeof(idt) - 1;
    idtd.base  = (uint64_t)&idt;

    // load with lidt
    idt_load(&idtd);
}
