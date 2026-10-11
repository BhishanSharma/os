// registry.h - the device tree (IOKit-lite)
//
// Every device the kernel knows is a node: the machine at the root, the PCI
// functions below it, and below those what their drivers found (an NVMe
// drive's partitions and volumes, USB devices, ...). PCI functions are matched
// against a table of drivers by class or vendor/device; drivers add their own
// nodes when they start. `ioreg` (a program) prints the tree.
#ifndef REGISTRY_H
#define REGISTRY_H

#include <stdint.h>

typedef struct {
    int used, parent;             /* parent -1: the root */
    char name[32];
    char cls[16];
    char info[96];
    uint16_t bus, slot, func;     /* PCI nodes */
} reg_node_t;

/* Add a node; returns its id (or -1 when the tree is full). printf-style info. */
int registry_add(int parent, const char *name, const char *cls, const char *fmt, ...);
/* The PCI tree: one node per function, with the driver that matches it. */
void registry_scan_pci(void);
/* The node of PCI function bus:slot.func, or the root (0) if there is none. */
int registry_pci_node(uint16_t bus, uint16_t slot, uint16_t func);
/* The first node of class `cls` (e.g. "storage"), or 0 (the root). */
int registry_find(const char *cls);
const reg_node_t *registry_node(int id);   /* 0 past the end */

#endif
