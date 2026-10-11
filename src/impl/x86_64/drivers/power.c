// power.c - ACPI power management through uACPI (see drivers/power.h)
//
// The firmware describes power off, the power button and the battery in AML
// bytecode; uACPI (third_party/uacpi) runs it. This file is what uACPI needs
// from a kernel (memory mapping, port and PCI access, locks, time), the
// embedded controller (the laptop's little helper chip that the battery
// methods read), and the polled event loop: the ACPI interrupt (SCI) is not
// wired up, power_poll() checks the event registers from the idle loop.
#include "drivers/power.h"
#include "drivers/acpi.h"
#include "drivers/paging.h"
#include "drivers/heap.h"
#include "drivers/pci.h"
#include "drivers/timer.h"
#include "sys/task.h"
#include "sys/smp.h"
#include "lib/print.h"
#include "lib/serial.h"
#include "lib/string.h"
#include "../lib/ports.h"

#include <uacpi/uacpi.h>
#include <uacpi/kernel_api.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/utilities.h>
#include <uacpi/resources.h>
#include <uacpi/opregion.h>
#include <uacpi/notify.h>
#include <uacpi/tables.h>
#include <uacpi/namespace.h>
#include <uacpi/acpi.h>

/* ---- Time ------------------------------------------------------------------ */

static uint64_t tsc_per_us, tsc_base;

static uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

/* The TSC rate, measured against the timer ticks. */
static void calibrate(void) {
    uint32_t t = get_tick();
    while (get_tick() == t) __asm__ volatile("pause");
    uint64_t a = rdtsc();
    t = get_tick();
    while ((uint32_t)(get_tick() - t) < 5) __asm__ volatile("pause");
    tsc_per_us = (rdtsc() - a) / (5 * 1000000 / TIMER_FREQ);
    if (!tsc_per_us) tsc_per_us = 1;
    tsc_base = rdtsc();
}

uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    return (rdtsc() - tsc_base) * 1000 / tsc_per_us;
}

void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t end = rdtsc() + tsc_per_us * usec;
    while (rdtsc() < end) __asm__ volatile("pause");
}

void uacpi_kernel_sleep(uacpi_u64 msec) {
    uint64_t end = uacpi_kernel_get_nanoseconds_since_boot() + msec * 1000000;
    while (uacpi_kernel_get_nanoseconds_since_boot() < end) cpu_wait();
}

/* ---- Memory ------------------------------------------------------------------ */

uacpi_status uacpi_kernel_get_rsdp(uacpi_phys_addr *out) {
    *out = acpi_rsdp_phys();
    return *out ? UACPI_STATUS_OK : UACPI_STATUS_NOT_FOUND;
}

/* Mappings are never taken down (another core could still hold a stale
 * translation), so they are kept and handed out again: uACPI maps the same
 * tables and registers over and over. */
#define MAPS 512
static struct { uint64_t phys, len; uint8_t *virt; } maps[MAPS];
static int map_count, map_next;

void *uacpi_kernel_map(uacpi_phys_addr addr, uacpi_size len) {
    for (int i = 0; i < map_count; i++)
        if (addr >= maps[i].phys && addr + len <= maps[i].phys + maps[i].len)
            return maps[i].virt + (addr - maps[i].phys);
    uint64_t first = addr & ~0xFFFull, last = (addr + len + 0xFFF) & ~0xFFFull;
    uint8_t *v = mmio_map(first, last - first);
    int i = map_count < MAPS ? map_count++ : map_next++ % MAPS;
    maps[i].phys = first;
    maps[i].len = last - first;
    maps[i].virt = v;
    return v + (addr - first);
}

void uacpi_kernel_unmap(void *addr, uacpi_size len) {}

void *uacpi_kernel_alloc(uacpi_size size) { return kmalloc(size); }

void uacpi_kernel_free(void *mem) {
    if (mem) kfree(mem);
}

/* ---- Log: the serial line only (the screen gets a summary) ------------------ */

void uacpi_kernel_log(uacpi_log_level level, const uacpi_char *msg) {
    serial_puts(level <= UACPI_LOG_WARN ? "acpi! " : "acpi: ");
    serial_puts(msg);
}

/* ---- Ports and PCI ------------------------------------------------------------ */

/* I/O "mappings" are the base port, offset so that port 0 is not a null handle. */
#define IO_HANDLE(base) ((uacpi_handle)((uint64_t)(base) + 0x10000))
#define IO_PORT(h, off) ((uint16_t)((uint64_t)(h) - 0x10000 + (off)))

uacpi_status uacpi_kernel_io_map(uacpi_io_addr base, uacpi_size len, uacpi_handle *out) {
    *out = IO_HANDLE(base);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_io_unmap(uacpi_handle h) {}

uacpi_status uacpi_kernel_io_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) { *v = inb(IO_PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) { *v = inw(IO_PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) { *v = inl(IO_PORT(h, off)); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) { outb(IO_PORT(h, off), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) { outw(IO_PORT(h, off), v); return UACPI_STATUS_OK; }
uacpi_status uacpi_kernel_io_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) { outl(IO_PORT(h, off), v); return UACPI_STATUS_OK; }

/* PCI devices: bus, device and function packed into the handle (plus one). */
uacpi_status uacpi_kernel_pci_device_open(uacpi_pci_address a, uacpi_handle *out) {
    if (a.segment != 0) return UACPI_STATUS_UNIMPLEMENTED;
    *out = (uacpi_handle)((uint64_t)a.bus << 16 | (uint64_t)a.device << 8 | a.function | 1ull << 32);
    return UACPI_STATUS_OK;
}
void uacpi_kernel_pci_device_close(uacpi_handle h) {}

#define PCI_BUS(h)  ((uint8_t)((uint64_t)(h) >> 16))
#define PCI_DEV(h)  ((uint8_t)((uint64_t)(h) >> 8))
#define PCI_FN(h)   ((uint8_t)(uint64_t)(h))

static uacpi_status pci_read(uacpi_handle h, uacpi_size off, int bytes, uint32_t *v) {
    if (off + bytes > 256) return UACPI_STATUS_UNIMPLEMENTED;     /* no extended space */
    uint32_t d = pci_config_read_dword(PCI_BUS(h), PCI_DEV(h), PCI_FN(h), (uint8_t)(off & ~3));
    d >>= (off & 3) * 8;
    *v = bytes == 4 ? d : d & ((1u << (bytes * 8)) - 1);
    return UACPI_STATUS_OK;
}

static uacpi_status pci_write(uacpi_handle h, uacpi_size off, int bytes, uint32_t v) {
    if (off + bytes > 256) return UACPI_STATUS_UNIMPLEMENTED;
    uint8_t reg = (uint8_t)(off & ~3);
    uint32_t d = v;
    if (bytes < 4) {
        int shift = (off & 3) * 8;
        uint32_t mask = ((1u << (bytes * 8)) - 1) << shift;
        d = (pci_config_read_dword(PCI_BUS(h), PCI_DEV(h), PCI_FN(h), reg) & ~mask) | ((v << shift) & mask);
    }
    pci_config_write_dword(PCI_BUS(h), PCI_DEV(h), PCI_FN(h), reg, d);
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_pci_read8(uacpi_handle h, uacpi_size off, uacpi_u8 *v) {
    uint32_t x = 0; uacpi_status s = pci_read(h, off, 1, &x); *v = (uacpi_u8)x; return s;
}
uacpi_status uacpi_kernel_pci_read16(uacpi_handle h, uacpi_size off, uacpi_u16 *v) {
    uint32_t x = 0; uacpi_status s = pci_read(h, off, 2, &x); *v = (uacpi_u16)x; return s;
}
uacpi_status uacpi_kernel_pci_read32(uacpi_handle h, uacpi_size off, uacpi_u32 *v) {
    return pci_read(h, off, 4, v);
}
uacpi_status uacpi_kernel_pci_write8(uacpi_handle h, uacpi_size off, uacpi_u8 v) { return pci_write(h, off, 1, v); }
uacpi_status uacpi_kernel_pci_write16(uacpi_handle h, uacpi_size off, uacpi_u16 v) { return pci_write(h, off, 2, v); }
uacpi_status uacpi_kernel_pci_write32(uacpi_handle h, uacpi_size off, uacpi_u32 v) { return pci_write(h, off, 4, v); }

/* ---- Locks and events ------------------------------------------------------------ */

/* Mutexes and events wait with cpu_wait(), which lets the kernel lock go, so
 * another core can make progress meanwhile. 0xFFFF is "forever". */
typedef struct { volatile int value; } counter_t;

static uacpi_handle new_counter(void) {
    counter_t *c = kmalloc(sizeof(*c));
    if (c) c->value = 0;
    return c;
}

uacpi_handle uacpi_kernel_create_mutex(void) { return new_counter(); }
void uacpi_kernel_free_mutex(uacpi_handle h) { kfree(h); }
uacpi_handle uacpi_kernel_create_event(void) { return new_counter(); }
void uacpi_kernel_free_event(uacpi_handle h) { kfree(h); }

static int timed_out(uint64_t start, uacpi_u16 timeout) {
    if (timeout == 0xFFFF) return 0;
    return uacpi_kernel_get_nanoseconds_since_boot() - start >= (uint64_t)timeout * 1000000;
}

uacpi_status uacpi_kernel_acquire_mutex(uacpi_handle h, uacpi_u16 timeout) {
    counter_t *m = h;
    uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
    while (!__sync_bool_compare_and_swap(&m->value, 0, 1)) {
        if (timeout == 0 || timed_out(start, timeout)) return UACPI_STATUS_TIMEOUT;
        cpu_wait();
    }
    return UACPI_STATUS_OK;
}

void uacpi_kernel_release_mutex(uacpi_handle h) {
    __atomic_store_n(&((counter_t *)h)->value, 0, __ATOMIC_RELEASE);
}

uacpi_bool uacpi_kernel_wait_for_event(uacpi_handle h, uacpi_u16 timeout) {
    counter_t *e = h;
    uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
    for (;;) {
        int v = e->value;
        if (v > 0 && __sync_bool_compare_and_swap(&e->value, v, v - 1)) return UACPI_TRUE;
        if (v > 0) continue;
        if (timeout == 0 || timed_out(start, timeout)) return UACPI_FALSE;
        cpu_wait();
    }
}

void uacpi_kernel_signal_event(uacpi_handle h) { __sync_fetch_and_add(&((counter_t *)h)->value, 1); }
void uacpi_kernel_reset_event(uacpi_handle h) { __atomic_store_n(&((counter_t *)h)->value, 0, __ATOMIC_RELEASE); }

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    task_t *t = task_current();
    return t ? (uacpi_thread_id)t : (uacpi_thread_id)1;
}

uacpi_interrupt_state uacpi_kernel_disable_interrupts(void) {
    uint64_t flags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void uacpi_kernel_restore_interrupts(uacpi_interrupt_state state) {
    if (state & 0x200) __asm__ volatile("sti" : : : "memory");
}

uacpi_handle uacpi_kernel_create_spinlock(void) { return new_counter(); }
void uacpi_kernel_free_spinlock(uacpi_handle h) { kfree(h); }

uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle h) {
    uacpi_cpu_flags flags = uacpi_kernel_disable_interrupts();
    while (!__sync_bool_compare_and_swap(&((counter_t *)h)->value, 0, 1)) __asm__ volatile("pause");
    return flags;
}

void uacpi_kernel_unlock_spinlock(uacpi_handle h, uacpi_cpu_flags flags) {
    __atomic_store_n(&((counter_t *)h)->value, 0, __ATOMIC_RELEASE);
    uacpi_kernel_restore_interrupts(flags);
}

uacpi_status uacpi_kernel_handle_firmware_request(uacpi_firmware_request *req) {
    return UACPI_STATUS_OK;                          /* breakpoints and fatal errors: carry on */
}

/* ---- The ACPI interrupt, polled, and deferred work ---------------------------------- */

static uacpi_interrupt_handler sci_handler;
static uacpi_handle sci_ctx;

uacpi_status uacpi_kernel_install_interrupt_handler(uacpi_u32 irq, uacpi_interrupt_handler handler,
                                                    uacpi_handle ctx, uacpi_handle *out) {
    sci_handler = handler;
    sci_ctx = ctx;
    *out = (uacpi_handle)1;
    return UACPI_STATUS_OK;
}

uacpi_status uacpi_kernel_uninstall_interrupt_handler(uacpi_interrupt_handler handler, uacpi_handle h) {
    sci_handler = 0;
    return UACPI_STATUS_OK;
}

#define WORK_SLOTS 32
static struct { uacpi_work_handler fn; uacpi_handle ctx; } work[WORK_SLOTS];
static volatile unsigned work_head, work_tail;

uacpi_status uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_work_handler fn, uacpi_handle ctx) {
    if (work_head - work_tail >= WORK_SLOTS) return UACPI_STATUS_OUT_OF_MEMORY;
    work[work_head % WORK_SLOTS].fn = fn;
    work[work_head % WORK_SLOTS].ctx = ctx;
    work_head++;
    return UACPI_STATUS_OK;
}

static void run_work(void) {
    while (work_tail != work_head) {
        unsigned i = work_tail++ % WORK_SLOTS;
        work[i].fn(work[i].ctx);
    }
}

uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    run_work();
    return UACPI_STATUS_OK;
}

/* ---- The embedded controller ------------------------------------------------------- */

#define EC_OBF      0x01        /* output buffer full: a byte to read */
#define EC_IBF      0x02        /* input buffer full: it has not taken the last byte */
#define EC_SCI_EVT  0x20        /* it has an event for us (a _Qxx method to run) */
#define EC_READ     0x80
#define EC_WRITE    0x81
#define EC_QUERY    0x84

static uint16_t ec_data, ec_cmd;
static uacpi_namespace_node *ec_node;
static int ec_up;

static int ec_wait(uint8_t mask, uint8_t want) {
    uint64_t end = uacpi_kernel_get_nanoseconds_since_boot() + 100000000;   /* 100 ms */
    while ((inb(ec_cmd) & mask) != want)
        if (uacpi_kernel_get_nanoseconds_since_boot() > end) return -1;
    return 0;
}

static int ec_command(uint8_t cmd) {
    if (inb(ec_cmd) & EC_OBF) (void)inb(ec_data);  /* a stale byte */
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_cmd, cmd);
    return ec_wait(EC_IBF, 0);
}

static int ec_read(uint8_t addr, uint8_t *v) {
    if (ec_command(EC_READ)) return -1;
    outb(ec_data, addr);
    if (ec_wait(EC_OBF, EC_OBF)) return -1;
    *v = inb(ec_data);
    return 0;
}

static int ec_write(uint8_t addr, uint8_t v) {
    if (ec_command(EC_WRITE)) return -1;
    outb(ec_data, addr);
    if (ec_wait(EC_IBF, 0)) return -1;
    outb(ec_data, v);
    return ec_wait(EC_IBF, 0);
}

static uacpi_status ec_handler(uacpi_region_op op, uacpi_handle data) {
    if (op == UACPI_REGION_OP_ATTACH || op == UACPI_REGION_OP_DETACH) return UACPI_STATUS_OK;
    if (op != UACPI_REGION_OP_READ && op != UACPI_REGION_OP_WRITE) return UACPI_STATUS_INVALID_ARGUMENT;
    uacpi_region_rw_data *rw = data;
    if (op == UACPI_REGION_OP_READ) rw->value = 0;
    for (int i = 0; i < rw->byte_width; i++) {
        uint8_t addr = (uint8_t)(rw->offset + i), b;
        if (op == UACPI_REGION_OP_READ) {
            if (ec_read(addr, &b)) return UACPI_STATUS_HARDWARE_TIMEOUT;
            rw->value |= (uint64_t)b << (8 * i);
        } else if (ec_write(addr, (uint8_t)(rw->value >> (8 * i)))) {
            return UACPI_STATUS_HARDWARE_TIMEOUT;
        }
    }
    return UACPI_STATUS_OK;
}

/* Its ports: the first two I/O resources of _CRS (data, then command). */
static uacpi_iteration_decision ec_port(void *user, uacpi_resource *r) {
    int *n = user;
    uint16_t port;
    if (r->type == UACPI_RESOURCE_TYPE_IO) port = r->io.minimum;
    else if (r->type == UACPI_RESOURCE_TYPE_FIXED_IO) port = r->fixed_io.address;
    else return UACPI_ITERATION_DECISION_CONTINUE;
    if ((*n)++ == 0) ec_data = port;
    else ec_cmd = port;
    return *n >= 2 ? UACPI_ITERATION_DECISION_BREAK : UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision ec_found(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    int n = 0;
    if (uacpi_for_each_device_resource(node, "_CRS", ec_port, &n) == UACPI_STATUS_OK && n >= 2) {
        ec_node = node;
        return UACPI_ITERATION_DECISION_BREAK;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

/* The ECDT table names it early; otherwise it is the PNP0C09 device. */
static void ec_find(void) {
    uacpi_table t;
    if (uacpi_table_find_by_signature("ECDT", &t) == UACPI_STATUS_OK) {
        struct acpi_ecdt *e = t.ptr;
        if (e->ec_control.address && e->ec_data.address &&
            uacpi_namespace_node_find(UACPI_NULL, e->ec_id, &ec_node) == UACPI_STATUS_OK) {
            ec_cmd = (uint16_t)e->ec_control.address;
            ec_data = (uint16_t)e->ec_data.address;
        }
        uacpi_table_unref(&t);
    }
    if (!ec_node) uacpi_find_devices("PNP0C09", ec_found, UACPI_NULL);
    if (!ec_node) return;
    if (uacpi_install_address_space_handler(ec_node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
                                            ec_handler, UACPI_NULL) == UACPI_STATUS_OK)
        ec_up = 1;
}

/* Events the controller has (a key, the charger plugged in, ...): run _Qxx. */
static void ec_events(void) {
    for (int i = 0; ec_up && i < 8 && (inb(ec_cmd) & EC_SCI_EVT); i++) {
        if (ec_command(EC_QUERY) || ec_wait(EC_OBF, EC_OBF)) return;
        uint8_t q = inb(ec_data);
        if (!q) return;
        static const char hex[] = "0123456789ABCDEF";
        char method[5] = { '_', 'Q', hex[q >> 4], hex[q & 15], 0 };
        uacpi_execute_simple(ec_node, method);
    }
}

/* ---- Batteries and the AC adapter ------------------------------------------------------ */

#define MAX_BATTERIES 2
#define UNKNOWN 0xFFFFFFFFull

typedef struct {
    uacpi_namespace_node *node;
    int present, have_info;
    int mwh;                              /* units: mWh/mW, or mAh/mA */
    uint64_t design, full, voltage_design;
    uint64_t state, rate, remaining, voltage;
    char model[24], type[12], maker[24];
} battery_t;

static battery_t bats[MAX_BATTERIES];
static int bat_count;
static uacpi_namespace_node *ac_node;
static int ac_online = -1;
static volatile int refresh_wanted = 1, button_pressed;
static uint32_t last_refresh, ready_since;
static int acpi_ready;

static uint64_t pkg_int(uacpi_object_array *a, size_t i) {
    uint64_t v = UNKNOWN;
    if (i < a->count) uacpi_object_get_integer(a->objects[i], &v);
    return v;
}

static void pkg_str(uacpi_object_array *a, size_t i, char *out, size_t len) {
    out[0] = 0;
    uacpi_data_view view;
    if (i >= a->count || uacpi_object_get_string_or_buffer(a->objects[i], &view) != UACPI_STATUS_OK) return;
    size_t n = 0;
    for (; n + 1 < len && n < view.length && view.const_text[n]; n++)
        out[n] = view.const_text[n] >= ' ' && view.const_text[n] < 127 ? view.const_text[n] : '?';
    out[n] = 0;
    while (n && out[n - 1] == ' ') out[--n] = 0;
}

/* _BIX (ACPI 4+) or _BIF: what the battery is. */
static void battery_info(battery_t *b) {
    uacpi_object *o;
    uacpi_object_array a;
    size_t base;
    if (uacpi_eval_simple_package(b->node, "_BIX", &o) == UACPI_STATUS_OK) {
        base = 1;                                     /* _BIX starts with a revision */
    } else if (uacpi_eval_simple_package(b->node, "_BIF", &o) == UACPI_STATUS_OK) {
        base = 0;
    } else {
        return;
    }
    if (uacpi_object_get_package(o, &a) == UACPI_STATUS_OK) {
        b->mwh = pkg_int(&a, base) == 0;
        b->design = pkg_int(&a, base + 1);
        b->full = pkg_int(&a, base + 2);
        b->voltage_design = pkg_int(&a, base + 4);
        size_t strings = base ? 16 : 9;               /* model number, serial, type, maker */
        pkg_str(&a, strings, b->model, sizeof(b->model));
        pkg_str(&a, strings + 2, b->type, sizeof(b->type));
        pkg_str(&a, strings + 3, b->maker, sizeof(b->maker));
        b->have_info = 1;
    }
    uacpi_object_unref(o);
}

static void battery_status(battery_t *b) {
    uacpi_u32 sta = 0;
    b->present = uacpi_eval_sta(b->node, &sta) == UACPI_STATUS_OK && (sta & 0x10);
    if (!b->present) return;
    if (!b->have_info || b->full == 0 || b->full == UNKNOWN) battery_info(b);
    uacpi_object *o;
    uacpi_object_array a;
    if (uacpi_eval_simple_package(b->node, "_BST", &o) != UACPI_STATUS_OK) return;
    if (uacpi_object_get_package(o, &a) == UACPI_STATUS_OK) {
        b->state = pkg_int(&a, 0);
        b->rate = pkg_int(&a, 1);
        b->remaining = pkg_int(&a, 2);
        b->voltage = pkg_int(&a, 3);
    }
    uacpi_object_unref(o);
}

static void refresh(void) {
    for (int i = 0; i < bat_count; i++) battery_status(&bats[i]);
    uint64_t psr;
    if (ac_node && uacpi_eval_simple_integer(ac_node, "_PSR", &psr) == UACPI_STATUS_OK) ac_online = psr != 0;
    last_refresh = get_tick();
    refresh_wanted = 0;
}

static int percent(const battery_t *b) {
    if (!b->present || b->full == 0 || b->full == UNKNOWN || b->remaining == UNKNOWN) return -1;
    uint64_t p = (b->remaining * 100 + b->full / 2) / b->full;
    return p > 100 ? 100 : (int)p;
}

/* Something changed (a battery, the charger): read them again soon. */
static uacpi_status changed(uacpi_handle ctx, uacpi_namespace_node *node, uacpi_u64 value) {
    refresh_wanted = 1;
    if (ctx == (uacpi_handle)2 && value == 0x80) button_pressed = 1;     /* a power button device */
    return UACPI_STATUS_OK;
}

static uacpi_iteration_decision found_battery(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    if (bat_count < MAX_BATTERIES) {
        bats[bat_count++].node = node;
        uacpi_install_notify_handler(node, changed, (uacpi_handle)1);
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_iteration_decision found_ac(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    ac_node = node;
    uacpi_install_notify_handler(node, changed, (uacpi_handle)1);
    return UACPI_ITERATION_DECISION_BREAK;
}

static uacpi_iteration_decision found_button(void *user, uacpi_namespace_node *node, uacpi_u32 depth) {
    uacpi_install_notify_handler(node, changed, (uacpi_handle)2);
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static uacpi_interrupt_ret power_button(uacpi_handle ctx) {
    button_pressed = 1;
    return UACPI_INTERRUPT_HANDLED;
}

/* ---- The interface ------------------------------------------------------------------------ */

int power_init(char *how, int how_len) {
    if (!acpi_rsdp_phys()) {
        k_snprintf(how, how_len, "no ACPI tables");
        return -1;
    }
    calibrate();
    uacpi_status st = uacpi_initialize(0);
    if (st == UACPI_STATUS_OK) st = uacpi_namespace_load();
    if (st != UACPI_STATUS_OK) {
        k_snprintf(how, how_len, "ACPI did not start: %s", uacpi_status_to_string(st));
        return -1;
    }
    ec_find();                                        /* before _REG and _INI run */
    st = uacpi_namespace_initialize();
    if (st != UACPI_STATUS_OK) {
        k_snprintf(how, how_len, "ACPI did not start: %s", uacpi_status_to_string(st));
        return -1;
    }
    uacpi_finalize_gpe_initialization();
    uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, power_button, UACPI_NULL);
    uacpi_find_devices("PNP0C0C", found_button, UACPI_NULL);
    uacpi_find_devices("PNP0C0A", found_battery, UACPI_NULL);
    uacpi_find_devices("ACPI0003", found_ac, UACPI_NULL);
    refresh();
    acpi_ready = 1;
    ready_since = get_tick();
    button_pressed = 0;

    char bat[48] = "";
    int p = bat_count ? percent(&bats[0]) : -1;
    if (p >= 0)
        k_snprintf(bat, sizeof(bat), ", battery %d%%%s", p, (bats[0].state & 2) ? " charging" : "");
    else if (bat_count)
        k_snprintf(bat, sizeof(bat), ", battery not readable");
    k_snprintf(how, how_len, "ACPI (uACPI)%s%s, power button (`shutdown`, `battery`)",
               ec_up ? ", embedded controller" : "", bat);
    return 0;
}

void power_poll(void) {
    static int busy;
    static uint32_t last;
    if (!acpi_ready || busy) return;
    uint32_t now = get_tick();
    if ((uint32_t)(now - last) < TIMER_FREQ / 20) return;
    last = now;
    busy = 1;
    if (sci_handler) sci_handler(sci_ctx);            /* fixed events and GPEs */
    ec_events();
    run_work();                                       /* notifications from the above */
    if (refresh_wanted || (uint32_t)(now - last_refresh) > 15 * TIMER_FREQ) refresh();
    busy = 0;

    if (button_pressed) {
        button_pressed = 0;
        /* The press that switched the laptop on can still be pending. */
        if ((uint32_t)(now - ready_since) > 2 * TIMER_FREQ) {
            kprintf("\nPower button: shutting down.\n");
            power_off();
        }
    }
}

int power_off(void) {
    if (!acpi_ready) return -1;
    print_flush();
    if (uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5) != UACPI_STATUS_OK) return -1;
    __asm__ volatile("cli");
    uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
    __asm__ volatile("sti");
    return -1;
}

void power_battery_text(char *out, int len) {
    out[0] = 0;
    if (!bat_count) return;
    int p = percent(&bats[0]);
    if (p < 0) return;
    const char *what = (bats[0].state & 2) ? " charging" : (ac_online == 1 ? " AC" : "");
    k_snprintf(out, len, "BAT %d%%%s", p, what);
}

static void print_amount(uint64_t v, int mwh, const char *suffix) {
    if (v == UNKNOWN) kprintf("?");
    else kprintf("%u.%u %s%s", (uint32_t)(v / 1000), (uint32_t)(v % 1000 / 100), mwh ? "Wh" : "Ah", suffix);
}

void power_print(void) {
    if (!acpi_ready) {
        kprintf("ACPI is not running, so the battery cannot be read.\n");
        return;
    }
    refresh();
    if (!bat_count) kprintf("No battery.\n");
    for (int i = 0; i < bat_count; i++) {
        battery_t *b = &bats[i];
        kprintf("Battery %d", i + 1);
        if (b->model[0] || b->maker[0]) kprintf(" (%s%s%s%s%s)", b->maker, b->maker[0] ? " " : "", b->model,
                                                b->type[0] ? ", " : "", b->type);
        if (!b->present) {
            kprintf(": not inserted\n");
            continue;
        }
        int p = percent(b);
        const char *state = (b->state & 2) ? "charging" : (b->state & 1) ? "discharging" : "not charging";
        if (p >= 0) kprintf(": %d%%, %s%s\n", p, state, (b->state & 4) ? ", CRITICAL" : "");
        else kprintf(": %s\n", state);
        kprintf("  Holds ");
        print_amount(b->remaining, b->mwh, "");
        kprintf(" of ");
        print_amount(b->full, b->mwh, "");
        if (b->design != UNKNOWN && b->design && b->full != UNKNOWN) {
            kprintf(" (new: ");
            print_amount(b->design, b->mwh, "");
            kprintf(", health %u%%)", (uint32_t)(b->full * 100 / b->design));
        }
        kprintf("\n");
        if (b->rate != UNKNOWN && b->rate > 0) {
            kprintf("  Rate %u.%u %s", (uint32_t)(b->rate / 1000), (uint32_t)(b->rate % 1000 / 100), b->mwh ? "W" : "A");
            if (b->voltage != UNKNOWN) kprintf(" at %u.%u V", (uint32_t)(b->voltage / 1000), (uint32_t)(b->voltage % 1000 / 100));
            uint64_t minutes = UNKNOWN;
            if ((b->state & 1) && b->remaining != UNKNOWN) minutes = b->remaining * 60 / b->rate;
            else if ((b->state & 2) && b->remaining != UNKNOWN && b->full != UNKNOWN && b->full > b->remaining)
                minutes = (b->full - b->remaining) * 60 / b->rate;
            if (minutes != UNKNOWN)
                kprintf(", %s about %uh %02um", (b->state & 1) ? "empty in" : "full in",
                        (uint32_t)(minutes / 60), (uint32_t)(minutes % 60));
            kprintf("\n");
        }
    }
    if (ac_node) kprintf("Charger: %s\n", ac_online == 1 ? "plugged in" : ac_online == 0 ? "not plugged in" : "unknown");
    kprintf("Embedded controller: %s\n", ec_up ? "running" : "not found");
}
