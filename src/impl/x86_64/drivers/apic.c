#include "drivers/apic.h"
#include "drivers/paging.h"
#include "drivers/timer.h"
#include "lib/print.h"
#include "lib/string.h"
#include "../lib/ports.h"
#include <stdint.h>

/* Local APIC registers (offsets into its 4 KiB window; in x2APIC mode the
 * same register is MSR 0x800 + offset / 16). */
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_DIV  0x3E0

#define MSR_APIC_BASE   0x1B
#define APIC_BASE_X2    (1u << 10)
#define APIC_BASE_ON    (1u << 11)

#define LVT_MASKED      0x10000
#define LVT_PERIODIC    0x20000
#define DELIVERY_NMI    0x400
#define DELIVERY_EXTINT 0x700

#define TIMER_VECTOR    0x20         /* same vector as the PIC's IRQ 0 */
#define SPURIOUS_VECTOR 0xFF

#define PAGE_PCD 0x10
#define PAGE_PWT 0x08

static volatile uint32_t *lapic;     /* MMIO window (xAPIC mode) */
static int x2apic;
static int apic_ready;
static int apic_timer_on;

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint32_t lapic_read(uint32_t reg) {
    if (x2apic) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v) {
    if (x2apic) wrmsr(0x800 + (reg >> 4), v);
    else lapic[reg / 4] = v;
}

static int have_apic(void) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    return (d >> 9) & 1;
}

/* Turn the local APIC on (it may be off, or in x2APIC mode if the firmware
 * chose that). Returns 0 on success. */
static int apic_enable(void) {
    if (apic_ready) return 0;
    if (!have_apic()) return -1;
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if (!(base & APIC_BASE_ON)) {
        base |= APIC_BASE_ON;
        wrmsr(MSR_APIC_BASE, base);
    }
    x2apic = (base & APIC_BASE_X2) != 0;
    if (!x2apic) {
        uint64_t phys = base & 0xFFFFFF000ULL;
        map_page(phys, phys, PAGE_PRESENT | PAGE_RW | PAGE_PCD | PAGE_PWT);
        lapic = (volatile uint32_t *)phys;
    }
    lapic_write(LAPIC_TPR, 0);                          /* accept every priority */
    lapic_write(LAPIC_SVR, 0x100 | SPURIOUS_VECTOR);    /* software enable */
    apic_ready = 1;
    kprintf("Local APIC at %x, %s mode\n", (uint32_t)(base & 0xFFFFF000u), x2apic ? "x2APIC" : "xAPIC");
    return 0;
}

/* CMOS clock seconds, or -1 while the clock is updating. */
static int rtc_second(void) {
    outb(0x70, 0x0A);
    if (inb(0x71) & 0x80) return -1;
    outb(0x70, 0x00);
    return inb(0x71);
}

/* Wait until the CMOS clock's seconds change. Gives up after ~4e10 TSC
 * cycles (many seconds even on a fast CPU) in case the clock is stuck.
 * Returns 0 at the moment of the change. */
static int rtc_wait_edge(void) {
    int s = -1;
    uint64_t t0 = rdtsc();
    while (s < 0) {
        s = rtc_second();
        if (rdtsc() - t0 > 40000000000ULL) return -1;
    }
    for (;;) {
        int now = rtc_second();
        if (now >= 0 && now != s) return 0;
        if (rdtsc() - t0 > 40000000000ULL) return -1;
        __asm__ volatile("pause");
    }
}

/* Do timer ticks arrive? Waits for the first tick, at most one to two seconds
 * of CMOS clock time (so a working machine passes in about 10 ms). */
static int ticks_arrive(void) {
    uint32_t t0 = get_tick();
    int edges = 0, s = -1;
    uint64_t c0 = rdtsc();
    while (edges < 2) {
        if (get_tick() != t0) return 1;
        int now = rtc_second();
        if (now >= 0) {
            if (s >= 0 && now != s) edges++;
            s = now;
        }
        if (rdtsc() - c0 > 40000000000ULL) break;
        __asm__ volatile("pause");
    }
    return get_tick() != t0;
}

/* Is the PIT's channel 0 counting down? (Some Intel chipsets gate it off.) */
static int pit_counting(void) {
    outb(0x43, 0x00);                   /* latch channel 0 */
    uint16_t a = inb(0x40);
    a |= (uint16_t)inb(0x40) << 8;
    for (volatile int i = 0; i < 100000; i++) {}
    outb(0x43, 0x00);
    uint16_t b = inb(0x40);
    b |= (uint16_t)inb(0x40) << 8;
    return a != b;
}

/* "Virtual wire" mode: the 8259 PIC's output reaches the CPU through the
 * local APIC's LINT0 pin. UEFI firmware often leaves LINT0 masked. */
static int virtual_wire(void) {
    if (apic_enable() != 0) return -1;
    outb(0x22, 0x70);                   /* IMCR (old chipsets): send the PIC to the APIC */
    outb(0x23, 0x01);
    lapic_write(LAPIC_LVT_LINT0, DELIVERY_EXTINT);
    lapic_write(LAPIC_LVT_LINT1, DELIVERY_NMI);
    return 0;
}

/* Drive the ticks from the local APIC timer, measured against the CMOS
 * clock (one second). Returns 0 on success. */
static int apic_timer_start(uint32_t *per_second) {
    if (apic_enable() != 0) return -1;
    lapic_write(LAPIC_TIMER_DIV, 0x3);                  /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | TIMER_VECTOR);
    if (rtc_wait_edge() != 0) return -1;
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    if (rtc_wait_edge() != 0) return -1;
    uint32_t counted = 0xFFFFFFFFu - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);
    if (counted < TIMER_FREQ * 100) return -1;          /* not counting */
    *per_second = counted;
    outb(0x21, inb(0x21) | 0x01);                       /* PIC IRQ 0 off: one tick source */
    apic_timer_on = 1;
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | TIMER_VECTOR);
    lapic_write(LAPIC_TIMER_INIT, counted / TIMER_FREQ);
    return 0;
}

void apic_timer_ack(void) {
    if (apic_timer_on) lapic_write(LAPIC_EOI, 0);
}

int timer_check(char *how, int size) {
    if (ticks_arrive()) {
        k_snprintf(how, size, "PIT at %u Hz via 8259 PIC", TIMER_FREQ);
        return 0;
    }
    int counting = pit_counting();
    kprintf("No timer ticks through the 8259 PIC; PIT %s\n", counting ? "is counting" : "is not counting");

    if (counting && virtual_wire() == 0 && ticks_arrive()) {
        k_snprintf(how, size, "PIT at %u Hz via local APIC (%s, virtual wire)", TIMER_FREQ,
                   x2apic ? "x2APIC" : "xAPIC");
        return 0;
    }
    if (!counting) virtual_wire();      /* the keyboard still comes through the PIC */

    uint32_t per_second;
    if (apic_timer_start(&per_second) == 0 && ticks_arrive()) {
        k_snprintf(how, size, "local APIC timer at %u Hz (%u MHz bus; PIT %s)", TIMER_FREQ,
                   per_second * 16 / 1000000, counting ? "gives no interrupts" : "is switched off");
        return 0;
    }
    k_snprintf(how, size, "no timer interrupts (PIT %s, local APIC %s)",
               counting ? "counting" : "stopped", apic_ready ? (x2apic ? "x2APIC" : "xAPIC") : "missing");
    return -1;
}
