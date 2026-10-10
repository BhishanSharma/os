// acpi.c - find tables through the RSDP, scan AML for I2C resource descriptors
#include "drivers/acpi.h"
#include "drivers/paging.h"
#include "core/multiboot2.h"
#include "lib/print.h"
#include "lib/string.h"
#include <stdint.h>

/* Tables can lie anywhere in physical memory (often near the top of RAM,
 * in the 1-2 GiB user range), so they are looked at through a window of
 * kernel addresses at 512 GiB, unmapped again afterwards. */
#define WINDOW      0x0000008000000000ULL
#define WINDOW_MAX  (4u << 20)               /* 4 MiB: larger tables are skipped */

static uint64_t mapped_pages;
static uint8_t rsdp_copy[36];
static int have_rsdp;

void acpi_save_rsdp(void) {
    const uint8_t *r = mb2_get_rsdp();
    if (!r || memcmp(r, "RSD PTR ", 8) != 0) return;
    memcpy(rsdp_copy, r, r[15] >= 2 ? 36 : 20);
    have_rsdp = 1;
}

static void unmap_window(void) {
    for (uint64_t i = 0; i < mapped_pages; i++) unmap_page(WINDOW + i * 4096);
    mapped_pages = 0;
}

/* Map [phys, phys + len) into the window; returns its address there. */
static const uint8_t *map_window(uint64_t phys, uint64_t len) {
    unmap_window();
    if (len > WINDOW_MAX) return 0;
    uint64_t first = phys & ~0xFFFull, last = (phys + len + 0xFFF) & ~0xFFFull;
    for (uint64_t p = first; p < last; p += 4096) {
        map_page(WINDOW + mapped_pages * 4096, p, PAGE_PRESENT);
        mapped_pages++;
    }
    return (const uint8_t *)(WINDOW + (phys - first));
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

/* A table's length from its header (mapping just the header). */
static uint32_t table_length(uint64_t phys, char sig[5]) {
    const uint8_t *h = map_window(phys, 36);
    if (!h) return 0;
    for (int i = 0; i < 4; i++) sig[i] = (char)h[i];
    sig[4] = 0;
    return rd32(h + 4);
}

static int name_char(uint8_t c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

/* The name of the Device() (AML: 5B 82 PkgLength NameString) nearest
 * before `pos`. */
static void device_before(const uint8_t *t, uint32_t pos, char out[5]) {
    out[0] = 0;
    uint32_t stop = pos > 65536 ? pos - 65536 : 0;
    for (uint32_t i = pos; i-- > stop + 1;) {
        if (t[i - 1] != 0x5B || t[i] != 0x82) continue;
        uint32_t n = i + 1;
        n += 1 + (t[n] >> 6);                         /* PkgLength */
        while (t[n] == '\\' || t[n] == '^') n++;      /* root / parent prefixes */
        if (n + 4 > pos) continue;
        int ok = 1;
        for (int k = 0; k < 4; k++) ok &= name_char(t[n + k]);
        if (!ok) continue;
        for (int k = 0; k < 4; k++) out[k] = (char)t[n + k];
        out[4] = 0;
        return;
    }
}

/* I2cSerialBus(V2) large resource descriptors: 8E len16, revision,
 * source index, bus type 1, flags, type flags16, type revision, type data
 * length16 (>= 6): speed32, address16, [vendor data], then the controller
 * path as a string. */
static int scan_table(const uint8_t *t, uint32_t len, acpi_i2c_device_t *out, int n, int max) {
    for (uint32_t i = 36; i + 18 < len && n < max; i++) {
        if (t[i] != 0x8E) continue;
        uint32_t dlen = t[i + 1] | t[i + 2] << 8;
        if (dlen < 15 || dlen > 200 || i + 3 + dlen > len) continue;
        if (t[i + 5] != 1 || (t[i + 3] != 1 && t[i + 3] != 2)) continue;   /* I2C, revision 1 or 2 */
        uint32_t tlen = t[i + 10] | t[i + 11] << 8;
        if (tlen < 6 || 12 + tlen >= 3 + dlen) continue;
        const uint8_t *src = t + i + 12 + tlen;
        uint32_t src_max = 3 + dlen - 12 - tlen;
        if (src[0] != '\\' && src[0] != '^' && !name_char(src[0])) continue;
        acpi_i2c_device_t *d = &out[n];
        d->speed = rd32(t + i + 12);
        d->address = (uint16_t)(t[i + 16] | t[i + 17] << 8);
        uint32_t k = 0;
        for (; k < src_max && k < sizeof(d->controller) - 1 && src[k] >= 0x20 && src[k] < 0x7F; k++)
            d->controller[k] = (char)src[k];
        d->controller[k] = 0;
        device_before(t, i, d->device);
        /* The same template can appear twice (e.g. a method returning a copy). */
        int dup = 0;
        for (int j = 0; j < n; j++)
            if (out[j].address == d->address && !strcmp(out[j].controller, d->controller) &&
                !strcmp(out[j].device, d->device)) dup = 1;
        if (!dup) n++;
        i += 2 + dlen;
    }
    return n;
}

static int scan_physical(uint64_t phys, acpi_i2c_device_t *out, int n, int max) {
    char sig[5];
    uint32_t len = table_length(phys, sig);
    if (len < 36) return n;
    const uint8_t *t = map_window(phys, len);
    if (!t) {
        kprintf("ACPI: %s at %lx is %u bytes: too big to scan\n", sig, phys, len);
        return n;
    }
    n = scan_table(t, len, out, n, max);
    return n;
}

int acpi_find_i2c_devices(acpi_i2c_device_t *out, int max) {
    if (!have_rsdp) return -1;
    const uint8_t *rsdp = rsdp_copy;
    memset(out, 0, sizeof(*out) * (uint64_t)max);

    /* Root table: XSDT (64-bit entries) on ACPI 2.0+, else RSDT. */
    int xsdt = rsdp[15] >= 2 && rd64(rsdp + 24);
    uint64_t root = xsdt ? rd64(rsdp + 24) : rd32(rsdp + 16);
    char sig[5];
    uint32_t root_len = table_length(root, sig);
    if (root_len < 36 || root_len > 4096) { unmap_window(); return -1; }
    uint64_t tables[64];
    int count = 0;
    const uint8_t *r = map_window(root, root_len);
    for (uint32_t off = 36; off + (xsdt ? 8 : 4) <= root_len && count < 64; off += xsdt ? 8 : 4)
        tables[count++] = xsdt ? rd64(r + off) : rd32(r + off);

    int n = 0;
    for (int i = 0; i < count; i++) {
        uint32_t len = table_length(tables[i], sig);
        if (!strcmp(sig, "FACP") && len >= 44) {             /* the FADT points at the DSDT */
            const uint8_t *f = map_window(tables[i], len);
            uint64_t dsdt = len >= 148 && rd64(f + 140) ? rd64(f + 140) : rd32(f + 40);
            n = scan_physical(dsdt, out, n, max);
        } else if (!strcmp(sig, "SSDT")) {
            n = scan_physical(tables[i], out, n, max);
        }
    }
    unmap_window();
    return n;
}

/* The MADT ("APIC" table): one entry per core that the firmware enabled. */
int acpi_list_cpus(uint32_t *ids, int max) {
    if (!have_rsdp) return -1;
    const uint8_t *rsdp = rsdp_copy;
    int xsdt = rsdp[15] >= 2 && rd64(rsdp + 24);
    uint64_t root = xsdt ? rd64(rsdp + 24) : rd32(rsdp + 16);
    char sig[5];
    uint32_t root_len = table_length(root, sig);
    if (root_len < 36 || root_len > 4096) { unmap_window(); return -1; }
    uint64_t tables[64];
    int count = 0;
    const uint8_t *r = map_window(root, root_len);
    for (uint32_t off = 36; off + (xsdt ? 8 : 4) <= root_len && count < 64; off += xsdt ? 8 : 4)
        tables[count++] = xsdt ? rd64(r + off) : rd32(r + off);
    int n = -1;
    for (int i = 0; i < count && n < 0; i++) {
        uint32_t len = table_length(tables[i], sig);
        if (strcmp(sig, "APIC") || len < 44) continue;
        const uint8_t *t = map_window(tables[i], len);
        if (!t) break;
        n = 0;
        for (uint32_t off = 44; off + 2 <= len && n < max;) {
            uint8_t type = t[off], elen = t[off + 1];
            if (elen < 2 || off + elen > len) break;
            if (type == 0 && elen >= 8 && (rd32(t + off + 4) & 1)) ids[n++] = t[off + 3];
            else if (type == 9 && elen >= 16 && (rd32(t + off + 8) & 1)) {
                uint32_t id = rd32(t + off + 4);
                int dup = 0;
                for (int k = 0; k < n; k++) dup |= ids[k] == id;
                if (!dup) ids[n++] = id;
            }
            off += elen;
        }
    }
    unmap_window();
    return n;
}
