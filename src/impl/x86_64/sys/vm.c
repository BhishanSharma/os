// vm.c - a program's memory regions, page faults, copy-on-write (sys/vm.h)
#include "sys/vm.h"
#include "sys/vfs.h"
#include "sys/task.h"
#include "drivers/paging.h"
#include "drivers/physmem.h"
#include "drivers/memory.h"
#include "drivers/heap.h"
#include "lib/string.h"

#define MMAP_TOP  0x00007F0000000000ULL       /* mappings go down from here (stack above) */

struct vm_object {
    int refs;
    int kind;
    uint64_t size;
    vfs_file_t file;                           /* VM_FILE */
    uint64_t *pages;                           /* VM_SHM: physical pages, 0 until touched */
    char name[32];                             /* VM_SHM */
    vm_object_t *next_shm;
};

static vm_object_t *shm_list;

void vm_init_space(vm_space_t *s) {
    s->regions = 0;
    s->mmap_next = MMAP_TOP;
}

static void obj_put(vm_object_t *o) {
    if (!o || --o->refs > 0) return;
    if (o->kind == VM_SHM) {
        for (vm_object_t **p = &shm_list; *p; p = &(*p)->next_shm)
            if (*p == o) { *p = o->next_shm; break; }
        for (uint64_t i = 0; i < o->size / PAGE_SIZE; i++)
            if (o->pages[i]) page_free(o->pages[i]);
        kfree(o->pages);
    }
    kfree(o);
}

vm_region_t *vm_add(vm_space_t *s, uint64_t start, uint64_t end, int prot, int kind, vm_object_t *obj) {
    vm_region_t *r = kmalloc(sizeof(*r));
    if (!r) return 0;
    r->start = start;
    r->end = end;
    r->prot = prot;
    r->kind = kind;
    r->obj = obj;
    r->next = s->regions;
    s->regions = r;
    return r;
}

vm_region_t *vm_find(vm_space_t *s, uint64_t addr) {
    for (vm_region_t *r = s->regions; r; r = r->next)
        if (addr >= r->start && addr < r->end) return r;
    return 0;
}

/* A zeroed page for a program: from all of RAM, else from the heap. */
static uint64_t new_page(int shareable) {
    uint64_t phys = page_alloc();
    if (!phys && !shareable) {
        void *frame = alloc_frame();
        if (frame) phys = virt_to_phys(frame);
    }
    if (phys) memset(phys_to_virt(phys), 0, PAGE_SIZE);
    return phys;
}

static void drop_page(uint64_t phys) {
    if (page_owned(phys)) page_free(phys);
    else free_frame(phys_to_virt(phys));
}

int vm_fault(uint64_t root, vm_space_t *s, uint64_t addr, int write) {
    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    uint64_t *pte = paging_user_pte(root, page, 0);
    if (pte && (*pte & PAGE_PRESENT)) {
        if (!write || (*pte & PAGE_RW)) return 0;             /* (another thread got there first) */
        if (!(*pte & PAGE_COW)) return -1;                    /* read-only: a real fault */
        uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;
        if (page_refcount(phys) <= 1) {                       /* the last one: take it over */
            *pte = (*pte | PAGE_RW) & ~(uint64_t)PAGE_COW;
        } else {
            uint64_t copy = new_page(0);
            if (!copy) return -1;
            memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_SIZE);
            *pte = copy | PAGE_PRESENT | PAGE_RW | PAGE_USER;
            page_free(phys);                                  /* one mapping less */
        }
        __asm__ volatile("invlpg (%0)" :: "r"(page) : "memory");
        return 0;
    }
    vm_region_t *r = vm_find(s, page);
    if (!r || (write && !(r->prot & VM_WRITE))) return -1;
    uint64_t phys = 0;
    if (r->kind == VM_SHM) {
        uint64_t i = (page - r->start) / PAGE_SIZE;
        if (i >= r->obj->size / PAGE_SIZE) return -1;
        if (!r->obj->pages[i] && !(r->obj->pages[i] = new_page(1))) return -1;
        phys = r->obj->pages[i];
        page_ref(phys);                                       /* the object keeps its own */
    } else {
        if (!(phys = new_page(0))) return -1;
        if (r->kind == VM_FILE) {
            uint64_t off = page - r->start;
            if (off < r->obj->size) {
                uint64_t n = r->obj->size - off < PAGE_SIZE ? r->obj->size - off : PAGE_SIZE;
                kmutex_lock(&vfs_lock);
                int got = vfs_read_at(&r->obj->file, (uint32_t)off, phys_to_virt(phys), (uint32_t)n);
                kmutex_unlock(&vfs_lock);
                if (got < 0) {
                    drop_page(phys);
                    return -1;
                }
            }
        }
    }
    if (paging_map_user_prot(root, page, phys, (r->prot & VM_WRITE) != 0) != 0) {
        drop_page(phys);
        return -1;
    }
    return 0;
}

uint64_t vm_map(vm_space_t *s, uint64_t len, int prot, int kind, vm_object_t *obj) {
    len = (len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!len || len > (1ULL << 40)) return 0;
    uint64_t start = s->mmap_next - len - PAGE_SIZE;         /* a gap between mappings */
    if (!vm_add(s, start, start + len, prot, kind, obj)) return 0;
    s->mmap_next = start;
    return start;
}

int vm_unmap(uint64_t root, vm_space_t *s, uint64_t addr) {
    for (vm_region_t **p = &s->regions; *p; p = &(*p)->next) {
        vm_region_t *r = *p;
        if (r->start != addr) continue;
        for (uint64_t v = r->start; v < r->end; v += PAGE_SIZE) {
            uint64_t *pte = paging_user_pte(root, v, 0);
            if (pte && (*pte & PAGE_PRESENT)) {
                drop_page(*pte & 0x000FFFFFFFFFF000ULL);
                *pte = 0;
                __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
            }
        }
        *p = r->next;
        obj_put(r->obj);
        kfree(r);
        return 0;
    }
    return -1;
}

vm_object_t *vm_file_object(const char *abs_path) {
    vm_object_t *o = kmalloc(sizeof(*o));
    if (!o) return 0;
    memset(o, 0, sizeof(*o));
    kmutex_lock(&vfs_lock);
    int r = vfs_open_file(abs_path, &o->file);
    if (r == 0) o->size = vfs_file_size(&o->file);
    kmutex_unlock(&vfs_lock);
    if (r != 0) {
        kfree(o);
        return 0;
    }
    o->kind = VM_FILE;
    o->refs = 1;
    return o;
}

vm_object_t *vm_shm_object(const char *name, uint64_t size) {
    for (vm_object_t *o = shm_list; o; o = o->next_shm)
        if (!strcmp(o->name, name)) {
            o->refs++;
            return o;
        }
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (!size || size > (1ULL << 32)) return 0;
    vm_object_t *o = kmalloc(sizeof(*o));
    if (!o) return 0;
    memset(o, 0, sizeof(*o));
    o->pages = kmalloc(size / PAGE_SIZE * sizeof(uint64_t));
    if (!o->pages) {
        kfree(o);
        return 0;
    }
    memset(o->pages, 0, size / PAGE_SIZE * sizeof(uint64_t));
    o->kind = VM_SHM;
    o->size = size;
    o->refs = 1;
    kstrncpy(o->name, name, sizeof(o->name));
    o->next_shm = shm_list;
    shm_list = o;
    return o;
}

struct fork_ctx { uint64_t dst; vm_space_t *src; int failed; };

static void fork_page(uint64_t virt, uint64_t *pte, void *arg) {
    struct fork_ctx *c = arg;
    if (c->failed) return;
    uint64_t phys = *pte & 0x000FFFFFFFFFF000ULL;
    uint64_t *to = paging_user_pte(c->dst, virt, 1);
    if (!to) {
        c->failed = 1;
        return;
    }
    vm_region_t *r = vm_find(c->src, virt);
    if (!page_owned(phys)) {                                  /* a heap page: copied now */
        uint64_t copy = new_page(0);
        if (!copy) {
            c->failed = 1;
            return;
        }
        memcpy(phys_to_virt(copy), phys_to_virt(phys), PAGE_SIZE);
        *to = copy | (*pte & 0xFFF);
        return;
    }
    if ((*pte & PAGE_RW) && !(r && r->kind == VM_SHM))      /* both copy on their first write */
        *pte = (*pte & ~(uint64_t)PAGE_RW) | PAGE_COW;
    *to = *pte;
    page_ref(phys);
}

int vm_fork(uint64_t src_root, vm_space_t *src, uint64_t dst_root, vm_space_t *dst) {
    dst->regions = 0;
    dst->mmap_next = src->mmap_next;
    for (vm_region_t *r = src->regions; r; r = r->next) {
        if (r->obj) r->obj->refs++;
        if (!vm_add(dst, r->start, r->end, r->prot, r->kind, r->obj)) return -1;
    }
    struct fork_ctx c = { dst_root, src, 0 };
    paging_walk_user(src_root, fork_page, &c);
    paging_flush();                                           /* the parent's pages are read-only now */
    return c.failed ? -1 : 0;
}

void vm_free(vm_space_t *s) {
    while (s->regions) {
        vm_region_t *r = s->regions;
        s->regions = r->next;
        obj_put(r->obj);
        kfree(r);
    }
}
