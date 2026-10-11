// efi.c - UEFI runtime services (see drivers/efi.h)
//
// The firmware's runtime code and data stay where the firmware put them
// (physical addresses: Terminal OS never calls SetVirtualAddressMap). They
// can lie anywhere, also where user programs live (the lower half), so calls go
// through an address space of their own: the kernel's mappings plus identity
// mappings of the firmware's runtime regions (paging_create_identity_space),
// switched to for the length of a call, with interrupts off.
#include "drivers/efi.h"
#include "drivers/paging.h"
#include "core/multiboot2.h"
#include "sys/smp.h"
#include "lib/string.h"

const uint8_t efi_global_guid[16] = { 0x61, 0xDF, 0xE4, 0x8B, 0xCA, 0x93, 0xD2, 0x11,
                                      0xAA, 0x0D, 0x00, 0xE0, 0x98, 0x03, 0x2B, 0x8C };

#define EFI_MEMORY_RUNTIME     (1ull << 63)
#define EFI_MMIO               11
#define EFI_MMIO_PORT          12
#define MAX_REGIONS            128

static uint64_t system_table;
static paging_range_t regions[MAX_REGIONS];
static int region_count;
static uint64_t efi_root;

/* The runtime services table, as far as it is used here. */
typedef uint64_t (__attribute__((ms_abi)) *get_variable_fn)(const uint16_t *, const uint8_t *, uint32_t *, uint64_t *, void *);
typedef uint64_t (__attribute__((ms_abi)) *set_variable_fn)(const uint16_t *, const uint8_t *, uint32_t, uint64_t, const void *);
typedef void (__attribute__((ms_abi)) *reset_fn)(uint32_t, uint64_t, uint64_t, void *);

#define ST_RUNTIME_SERVICES    88      /* EFI_SYSTEM_TABLE.RuntimeServices */
#define RT_GET_VARIABLE        72      /* EFI_RUNTIME_SERVICES fields */
#define RT_SET_VARIABLE        88
#define RT_RESET_SYSTEM        104

void efi_save_boot_info(void) {
    const uint8_t *st = mb2_get_tag(12);                   /* the 64-bit system table */
    const uint8_t *mm = mb2_get_tag(17);                   /* the firmware's memory map */
    if (!st || !mm) return;
    uint32_t size = *(const uint32_t *)(mm + 4), desc = *(const uint32_t *)(mm + 8);
    if (desc < 40) return;
    for (uint32_t off = 16; off + desc <= size && region_count < MAX_REGIONS; off += desc) {
        const uint8_t *d = mm + off;
        uint32_t type = *(const uint32_t *)d;
        uint64_t start = *(const uint64_t *)(d + 8), pages = *(const uint64_t *)(d + 24);
        uint64_t attr = *(const uint64_t *)(d + 32);
        if (!(attr & EFI_MEMORY_RUNTIME)) continue;
        regions[region_count].start = start;
        regions[region_count].size = pages * 4096;
        regions[region_count].uncached = type == EFI_MMIO || type == EFI_MMIO_PORT;
        region_count++;
    }
    system_table = *(const uint64_t *)(st + 8);
}

int efi_available(void) {
    return system_table != 0 && region_count > 0;
}

/* Run `fn` with the firmware mapped. */
static uint64_t in_firmware(uint64_t (*fn)(void *), void *ctx) {
    if (!efi_available()) return ~0ull;
    if (!efi_root) efi_root = paging_create_identity_space(regions, region_count);
    if (!efi_root) return ~0ull;
    bkl_enter();
    uint64_t flags, old;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(flags) : : "memory");
    __asm__ volatile("mov %%cr3, %0" : "=r"(old));
    __asm__ volatile("mov %0, %%cr3" : : "r"(efi_root) : "memory");
    uint64_t r = fn(ctx);
    __asm__ volatile("mov %0, %%cr3" : : "r"(old) : "memory");
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
    bkl_leave();
    return r;
}

static uint64_t service(int offset) {
    uint64_t rt = *(volatile uint64_t *)(system_table + ST_RUNTIME_SERVICES);
    return *(volatile uint64_t *)(rt + offset);
}

typedef struct {
    uint16_t name[64];
    const uint8_t *guid;
    uint32_t attributes;
    uint64_t size;
    void *data;
    const void *cdata;
} var_call_t;

static void to_ucs2(const char *s, uint16_t *out, int max) {
    int i = 0;
    for (; s[i] && i < max - 1; i++) out[i] = (uint8_t)s[i];
    out[i] = 0;
}

static uint64_t do_get(void *ctx) {
    var_call_t *c = ctx;
    return ((get_variable_fn)service(RT_GET_VARIABLE))(c->name, c->guid, &c->attributes, &c->size, c->data);
}

static uint64_t do_set(void *ctx) {
    var_call_t *c = ctx;
    return ((set_variable_fn)service(RT_SET_VARIABLE))(c->name, c->guid, c->attributes, c->size, c->cdata);
}

int efi_get_variable(const char *name, const uint8_t guid[16], void *data, uint32_t size, uint32_t *attributes) {
    var_call_t c;
    to_ucs2(name, c.name, 64);
    c.guid = guid;
    c.attributes = 0;
    c.size = size;
    c.data = data;
    if (in_firmware(do_get, &c) != 0) return -1;
    if (attributes) *attributes = c.attributes;
    return (int)c.size;
}

int efi_set_variable(const char *name, const uint8_t guid[16], uint32_t attributes, const void *data, uint32_t size) {
    var_call_t c;
    to_ucs2(name, c.name, 64);
    c.guid = guid;
    c.attributes = attributes;
    c.size = size;
    c.cdata = data;
    return in_firmware(do_set, &c) == 0 ? 0 : -1;
}

static uint64_t do_reset(void *ctx) {
    ((reset_fn)service(RT_RESET_SYSTEM))(*(uint32_t *)ctx, 0, 0, 0);
    return ~0ull;
}

void efi_reset(int shutdown) {
    uint32_t type = shutdown ? 2 : 1;                       /* EfiResetShutdown, EfiResetWarm */
    in_firmware(do_reset, &type);
}
