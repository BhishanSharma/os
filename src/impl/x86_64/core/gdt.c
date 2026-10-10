#include "core/gdt.h"
#include "sys/smp.h"

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];          // rsp[0]: kernel stack for interrupts that arrive in ring 3
    uint64_t reserved1;
    uint64_t ist[7];          // interrupt stack table
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

extern void gdt_flush(const void* gdtr);     // gdt_load.asm

// null, code, data, TSS of core 0 (16 bytes), user code, user data, then a
// TSS (16 bytes) for each further core.
#define GDT_ENTRIES (7 + 2 * (MAX_CPUS - 1))
static uint64_t gdt[GDT_ENTRIES];
static struct tss tss[MAX_CPUS];
static uint8_t fatal_stack[8192] __attribute__((aligned(16)));   // core 0; the others get one from the heap

static uint16_t tss_selector(int cpu) {
    return (uint16_t)(cpu == 0 ? GDT_TSS : (7 + 2 * (cpu - 1)) * 8);
}

static void set_tss_descriptor(int cpu) {
    struct tss *t = &tss[cpu];
    t->iomap_base = sizeof(*t);               // no I/O permission bitmap
    uint64_t base = (uint64_t)t, limit = sizeof(*t) - 1;
    int i = tss_selector(cpu) / 8;
    gdt[i] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)                  // present, 64-bit TSS (available)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[i + 1] = base >> 32;
}

static void load(int cpu) {
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr = {
        sizeof(gdt) - 1, (uint64_t)gdt
    };
    gdt_flush(&gdtr);
    __asm__ volatile("ltr %0" : : "r"(tss_selector(cpu)));
}

void gdt_init(void) {
    gdt[0] = 0;
    gdt[1] = 0x0020980000000000ULL;           // 64-bit code: P, S, exec, L
    gdt[2] = 0x0000920000000000ULL;           // data: P, S, writable
    gdt[5] = 0x0020F80000000000ULL;           // user code: as gdt[1] with DPL 3
    gdt[6] = 0x0000F20000000000ULL;           // user data: as gdt[2] with DPL 3
    for (int cpu = 0; cpu < MAX_CPUS; cpu++) set_tss_descriptor(cpu);
    tss[0].ist[IST_FATAL - 1] = (uint64_t)(fatal_stack + sizeof(fatal_stack));
    load(0);
}

void gdt_init_ap(int cpu, uint64_t fatal_stack_top) {
    tss[cpu].ist[IST_FATAL - 1] = fatal_stack_top;
    load(cpu);
}

void gdt_set_kernel_stack(uint64_t top) {
    tss[cpu_index()].rsp[0] = top;
}
