// registry.c - the device tree (drivers/registry.h)
#include "drivers/registry.h"
#include "drivers/pci.h"
#include "drivers/heap.h"
#include "lib/string.h"
#include <stdarg.h>

#define MAX_NODES 128

static reg_node_t nodes[MAX_NODES];
static int count;

/* Which driver takes a PCI function: by class/subclass, or vendor/device. */
static const struct {
    uint8_t cls, sub;             /* 0xFF: any */
    uint16_t vendor, device;      /* 0: any */
    const char *driver;
} matches[] = {
    { 0x01, 0x08, 0, 0, "nvme" },
    { 0x01, 0x01, 0, 0, "ata" },
    { 0x0C, 0x03, 0, 0, "xhci (USB 3)" },
    { 0x04, 0x03, 0, 0, "hda (sound)" },
    { 0x02, 0x80, 0x8086, 0, "iwlwifi (Wi-Fi)" },
    { 0x02, 0x00, 0x10EC, 0x8139, "rtl8139" },
    { 0x02, 0x00, 0x10EC, 0x8168, "rtl8168" },
    { 0x02, 0x00, 0x8086, 0, "e1000" },
    { 0x03, 0xFF, 0, 0, "framebuffer console" },
    { 0x0C, 0x80, 0x8086, 0, "i2c-hid (touchpad)" },
    { 0x08, 0x80, 0x80EE, 0, "vmmdev (VirtualBox)" },
};

static void init_root(void) {
    if (count) return;
    nodes[0].used = 1;
    nodes[0].parent = -1;
    kstrncpy(nodes[0].name, "Terminal OS PC", sizeof(nodes[0].name));
    kstrncpy(nodes[0].cls, "machine", sizeof(nodes[0].cls));
    count = 1;
}

int registry_add(int parent, const char *name, const char *cls, const char *fmt, ...) {
    init_root();
    if (count == MAX_NODES) return -1;
    reg_node_t *n = &nodes[count];
    memset(n, 0, sizeof(*n));
    n->used = 1;
    n->parent = parent >= 0 && parent < count ? parent : 0;
    kstrncpy(n->name, name, sizeof(n->name));
    kstrncpy(n->cls, cls, sizeof(n->cls));
    va_list ap;
    va_start(ap, fmt);
    k_vsnprintf(n->info, sizeof(n->info), fmt, ap);
    va_end(ap);
    return count++;
}

void registry_scan_pci(void) {
    static pci_device_t devs[64];
    int n = pci_scan(devs, 64);
    int bus = registry_add(0, "PCI bus", "bus", "%d functions", n);
    for (int i = 0; i < n; i++) {
        pci_device_t *d = &devs[i];
        const char *driver = 0;
        for (unsigned k = 0; k < sizeof(matches) / sizeof(matches[0]) && !driver; k++)
            if ((matches[k].cls == d->class_code) &&
                (matches[k].sub == 0xFF || matches[k].sub == d->subclass) &&
                (!matches[k].vendor || matches[k].vendor == d->vendor) &&
                (!matches[k].device || matches[k].device == d->device))
                driver = matches[k].driver;
        const char *vendor = pci_vendor_name(d->vendor);
        char name[32];
        k_snprintf(name, sizeof(name), "%02x:%02x.%x", d->bus, d->slot, d->func);
        int id = registry_add(bus, name, "pci", "%s %04x:%04x %s%s%s", vendor ? vendor : "",
                              d->vendor, d->device, pci_class_name(d->class_code, d->subclass, d->prog_if),
                              driver ? ", driver " : "", driver ? driver : "");
        if (id >= 0) {
            nodes[id].bus = d->bus;
            nodes[id].slot = d->slot;
            nodes[id].func = d->func;
        }
    }
}

int registry_pci_node(uint16_t bus, uint16_t slot, uint16_t func) {
    for (int i = 0; i < count; i++)
        if (!strcmp(nodes[i].cls, "pci") && nodes[i].bus == bus && nodes[i].slot == slot && nodes[i].func == func)
            return i;
    return 0;
}

int registry_find(const char *cls) {
    for (int i = 0; i < count; i++)
        if (!strcmp(nodes[i].cls, cls)) return i;
    return 0;
}

const reg_node_t *registry_node(int id) {
    init_root();
    return id >= 0 && id < count ? &nodes[id] : 0;
}
