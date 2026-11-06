// cpupower.c - hardware P-states and idle C-states (see drivers/cpupower.h)
//
// Intel SDM vol. 3, "Power and Thermal Management": CPUID 6 (HWP, EPP,
// ARAT, APERF/MPERF), IA32_PM_ENABLE (0x770), IA32_HWP_CAPABILITIES (0x771),
// IA32_HWP_REQUEST (0x774), IA32_ENERGY_PERF_BIAS (0x1B0); CPUID 5 and
// MONITOR/MWAIT for C-states (MWAIT hint 0x20 is core C6 since Sandy Bridge,
// 0x10 C3, 0x00 C1).
#include "drivers/cpupower.h"
#include "drivers/timer.h"
#include "sys/smp.h"
#include "sys/sysinfo.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

#define MSR_MPERF        0xE7
#define MSR_APERF        0xE8
#define MSR_ENERGY_BIAS  0x1B0
#define MSR_PM_ENABLE    0x770
#define MSR_HWP_CAPS     0x771
#define MSR_HWP_REQUEST  0x774
#define EPP_BALANCED     0x80              /* 0 = performance ... 255 = power saving */
#define BIAS_BALANCED    6                 /* 0 = performance ... 15 = power saving */

static int intel, in_vm, has_hwp, has_epp, has_bias, has_aperf, has_mwait, has_arat, invariant_tsc;
static int idle_hint = -1;                 /* MWAIT hint for idle cores; -1: HLT */
static uint32_t base_mhz, max_mhz;
static uint8_t hwp_lowest, hwp_efficient, hwp_guaranteed, hwp_highest;
static char status[120];

/* One cache line per core for MONITOR (nothing writes them: an interrupt wakes). */
static volatile uint64_t wake_line[MAX_CPUS][8] __attribute__((aligned(64)));

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}

static void wrmsr(uint32_t msr, uint64_t v) {
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* This core: HWP on, from its lowest to its highest level (turbo), the CPU
 * choosing in between, leaning balanced. */
static void setup_core(void) {
    if (has_hwp) {
        wrmsr(MSR_PM_ENABLE, 1);                         /* (stays on until the next reset) */
        uint64_t caps = rdmsr(MSR_HWP_CAPS);
        uint64_t req = (caps >> 24 & 0xFF) | (caps & 0xFF) << 8;   /* min, max; desired 0: the CPU picks */
        if (has_epp) req |= (uint64_t)EPP_BALANCED << 24;
        wrmsr(MSR_HWP_REQUEST, req);
    }
    if (has_bias && !has_epp) wrmsr(MSR_ENERGY_BIAS, BIAS_BALANCED);
}

static const char *cstate_name(int hint) {
    return hint < 0 ? "C1 (HLT)" : hint >= 0x20 ? "C6" : hint >= 0x10 ? "C3" : "C1 (MWAIT)";
}

void cpupower_init(void) {
    uint32_t r[4];
    cpuid(0, 0, r);
    uint32_t max_leaf = r[0];
    intel = r[1] == 0x756E6547 && r[3] == 0x49656E69 && r[2] == 0x6C65746E;   /* "GenuineIntel" */
    cpuid(1, 0, r);
    in_vm = (r[2] >> 31) & 1;                            /* a hypervisor is there */
    has_mwait = (r[2] >> 3) & 1;
    if (max_leaf >= 6) {
        cpuid(6, 0, r);
        has_arat = (r[0] >> 2) & 1;
        has_hwp = (r[0] >> 7) & 1;
        has_epp = (r[0] >> 10) & 1;
        has_aperf = r[2] & 1;
        has_bias = (r[2] >> 3) & 1;
    }
    if (max_leaf >= 0x16) {
        cpuid(0x16, 0, r);
        base_mhz = r[0] & 0xFFFF;
        max_mhz = r[1] & 0xFFFF;
    }
    cpuid(0x80000000, 0, r);
    if (r[0] >= 0x80000007) {
        cpuid(0x80000007, 0, r);
        invariant_tsc = (r[3] >> 8) & 1;
    }
    if (in_vm || !intel) has_hwp = has_epp = has_bias = has_aperf = has_mwait = 0;   /* (the host decides) */
    /* The deepest of C1, C3, C6 the CPU lists (CPUID 5: sub-states per MWAIT
     * C-state), where the APIC timer and the TSC keep running. */
    if (has_mwait && has_arat && invariant_tsc && max_leaf >= 5) {
        cpuid(5, 0, r);
        if (r[2] & 1)
            for (int k = 2; k >= 0 && idle_hint < 0; k--)
                if ((r[3] >> (4 * (k + 1))) & 0xF) idle_hint = k << 4;
    }
    setup_core();
    if (has_hwp) {
        uint64_t caps = rdmsr(MSR_HWP_CAPS);
        hwp_highest = (uint8_t)caps;
        hwp_guaranteed = (uint8_t)(caps >> 8);
        hwp_efficient = (uint8_t)(caps >> 16);
        hwp_lowest = (uint8_t)(caps >> 24);
    }
    if (in_vm)
        k_snprintf(status, sizeof(status), "in a virtual machine: the host sets the clock; idle with HLT");
    else if (!intel)
        k_snprintf(status, sizeof(status), "not an Intel CPU: its clock is as the firmware set it; idle with HLT");
    else
        k_snprintf(status, sizeof(status), "%s, idle cores in %s (`cpu` shows the clock)",
                   has_hwp ? (has_epp ? "the CPU picks its clock (HWP, balanced)" : "the CPU picks its clock (HWP)")
                           : "no HWP: the clock is as the firmware set it",
                   cstate_name(idle_hint));
}

void cpupower_init_ap(void) { setup_core(); }

void cpu_idle_wait(void) {
    if (idle_hint < 0) {
        __asm__ volatile("sti; hlt");
        return;
    }
    __asm__ volatile("monitor" : : "a"(&wake_line[cpu_index()][0]), "c"(0), "d"(0));
    __asm__ volatile("sti; mwait" : : "a"((uint32_t)idle_hint), "c"(0) : "memory");
}

const char *cpupower_status(void) { return status; }

/* This core's clock while it works for `ms`: APERF/MPERF (MPERF counts at
 * the TSC's rate) times the TSC's rate. MHz, 0 if it cannot tell. */
static uint32_t busy_mhz(uint32_t ms) {
    if (!has_aperf) return 0;
    uint64_t a0 = rdmsr(MSR_APERF), m0 = rdmsr(MSR_MPERF), t0 = rdtsc();
    uint32_t start = timer_ms();
    while (timer_ms() - start < ms) __asm__ volatile("pause");
    uint64_t a = rdmsr(MSR_APERF) - a0, m = rdmsr(MSR_MPERF) - m0, t = rdtsc() - t0;
    uint32_t took = timer_ms() - start;
    if (!m || !took) return 0;
    uint64_t tsc_khz = t / took;
    return (uint32_t)(a * tsc_khz / m / 1000);
}

static void ghz(uint32_t mhz, char *out, int size) {
    k_snprintf(out, (size_t)size, "%u.%u GHz", mhz / 1000, mhz % 1000 / 100);
}

void cpupower_print(void) {
    char a[16], b[16];
    kprintf("CPU: %s, %d core%s running\n", sysinfo_cpu_name(), smp_cpu_count(), smp_cpu_count() == 1 ? "" : "s");
    if (base_mhz) {
        ghz(base_mhz, a, sizeof(a));
        ghz(max_mhz, b, sizeof(b));
        kprintf("  clock: base %s, up to %s (what the CPU says)\n", a, b);
    }
    kprintf("  %s\n", status);
    if (has_hwp)
        kprintf("  HWP levels: lowest %u, most efficient %u, guaranteed %u, highest %u%s\n", hwp_lowest, hwp_efficient,
                hwp_guaranteed, hwp_highest, has_epp ? "; preference balanced (128 of 0..255)" : "");
    uint32_t mhz = busy_mhz(100);
    if (mhz) {
        ghz(mhz, a, sizeof(a));
        kprintf("  core %d now: %s while busy (100 ms of work)\n", cpu_index(), a);
    } else {
        kprintf("  the clock now cannot be measured here (no APERF/MPERF%s)\n", in_vm ? " in a virtual machine" : "");
    }
}
