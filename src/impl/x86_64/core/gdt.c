#include "core/gdt.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];          // stacks for ring 0..2 (unused: everything is ring 0)
    uint64_t reserved1;
    uint64_t ist[7];          // interrupt stack table
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern void gdt_flush(const void* gdtr);     // gdt_load.asm

static uint64_t gdt[5];                       // null, code, data, TSS (16 bytes)
static struct tss tss;
static uint8_t fatal_stack[8192] __attribute__((aligned(16)));

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x0020980000000000ULL;           // 64-bit code: P, S, exec, L
    gdt[2] = 0x0000920000000000ULL;           // data: P, S, writable

    tss.ist[IST_FATAL - 1] = (uint64_t)(fatal_stack + sizeof(fatal_stack));
    tss.iomap_base = sizeof(tss);             // no I/O permission bitmap

    uint64_t base  = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[3] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)                  // present, 64-bit TSS (available)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[4] = base >> 32;

    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr = {
        sizeof(gdt) - 1, (uint64_t)gdt
    };
    gdt_flush(&gdtr);
    __asm__ volatile("ltr %0" : : "r"((uint16_t)GDT_TSS));
}
