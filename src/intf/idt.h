#ifndef IDT_H
#define IDT_H

#include <stdint.h>

struct IDTEntry {
    uint16_t offset_low;   // bits 0..15
    uint16_t selector;     // code segment selector in GDT
    uint8_t  ist;          // interrupt stack table
    uint8_t  type_attr;    // type and attributes
    uint16_t offset_mid;   // bits 16..31
    uint32_t offset_high;  // bits 32..63
    uint32_t zero;         // reserved
} __attribute__((packed));

struct IDTDescriptor {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_init();
void idt_set_entry(int vector, void* isr, uint8_t flags);

#endif
