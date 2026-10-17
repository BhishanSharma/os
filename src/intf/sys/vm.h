// vm.h - a program's memory: regions, filled in when first touched
//
// A program's address space is a list of regions (heap, stack, mappings).
// Pages appear in a region on the first access (vm_fault, from the page-fault
// handler or before a system call touches the memory): zeroed (anonymous),
// read from a file, or shared with every program mapping the same shared
// memory object. fork shares pages copy-on-write: both get them read-only
// with PAGE_COW, and the first write copies (or takes over) the page.
#ifndef VM_H
#define VM_H

#include <stdint.h>

#define VM_READ   1
#define VM_WRITE  2

enum { VM_ANON, VM_FILE, VM_SHM };

typedef struct vm_object vm_object_t;

typedef struct vm_region {
    uint64_t start, end;          /* page aligned, end exclusive */
    int prot;                     /* VM_READ | VM_WRITE */
    int kind;                     /* VM_ANON, VM_FILE, VM_SHM */
    vm_object_t *obj;             /* the file or shared memory (counted reference) */
    uint64_t offset;              /* bytes into obj at `start` (after a partial unmap) */
    struct vm_region *next;
} vm_region_t;

typedef struct {
    vm_region_t *regions;
    uint64_t mmap_next;           /* mappings go down from here */
} vm_space_t;

void vm_init_space(vm_space_t *s);
vm_region_t *vm_add(vm_space_t *s, uint64_t start, uint64_t end, int prot, int kind, vm_object_t *obj);
vm_region_t *vm_find(vm_space_t *s, uint64_t addr);

/* Make `addr` accessible (for writing if `write`): map the page if its region
 * has none yet, or resolve copy-on-write. 0 on success, -1: not allowed
 * (a real segmentation fault). */
int vm_fault(uint64_t root, vm_space_t *s, uint64_t addr, int write);

/* Map `len` bytes somewhere free: anonymous (obj 0), a file or shared memory.
 * Returns the address, 0 if there is no room. Takes over the reference. */
uint64_t vm_map(vm_space_t *s, uint64_t len, int prot, int kind, vm_object_t *obj);
int vm_unmap(uint64_t root, vm_space_t *s, uint64_t addr);   /* a whole mapping */
/* munmap(addr, len): the pages in [addr, addr + len), from any regions (which
 * shrink or split). 0 on success, -1 if the range is not page aligned. */
int vm_unmap_range(uint64_t root, vm_space_t *s, uint64_t addr, uint64_t len);

vm_object_t *vm_file_object(const char *abs_path);           /* 0 if it cannot be opened */
vm_object_t *vm_shm_object(const char *name, uint64_t size); /* new, or the existing one */

/* fork: copy the regions and share every page copy-on-write. 0 on success. */
int vm_fork(uint64_t src_root, vm_space_t *src, uint64_t dst_root, vm_space_t *dst);

/* Drop the regions (the pages go with the page tables). */
void vm_free(vm_space_t *s);

#endif
