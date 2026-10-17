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
    r->offset = 0;
    r->limit = 0;
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
        uint64_t i = (r->offset + page - r->start) / PAGE_SIZE;
        if (i >= r->obj->size / PAGE_SIZE) return -1;
        if (!r->obj->pages[i] && !(r->obj->pages[i] = new_page(1))) return -1;
        phys = r->obj->pages[i];
        page_ref(phys);                                       /* the object keeps its own */
    } else {
        if (!(phys = new_page(0))) return -1;
        if (r->kind == VM_FILE) {
            uint64_t off = r->offset + page - r->start;
            uint64_t end = r->limit && r->limit < r->obj->size ? r->limit : r->obj->size;
            if (off < end) {
                uint64_t n = end - off < PAGE_SIZE ? end - off : PAGE_SIZE;
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
    if (!len || len > (1ULL << 46)) return 0;           /* 64 TiB: reservations (JavaScript cages) */
    uint64_t start = s->mmap_next - len - PAGE_SIZE;         /* a gap between mappings */
    if (!vm_add(s, start, start + len, prot, kind, obj)) return 0;
    s->mmap_next = start;
    return start;
}

static void drop_range(uint64_t root, uint64_t lo, uint64_t hi) {
    for (uint64_t v = lo; v < hi; v += PAGE_SIZE) {
        uint64_t *pte = paging_user_pte(root, v, 0);
        if (!pte) {                                   /* no page table: 2 MiB with nothing mapped */
            v = (v | 0x1FFFFFULL) + 1 - PAGE_SIZE;
            continue;
        }
        if (*pte & PAGE_PRESENT) {
            drop_page(*pte & 0x000FFFFFFFFFF000ULL);
            *pte = 0;
            __asm__ volatile("invlpg (%0)" :: "r"(v) : "memory");
        }
    }
}

int vm_unmap_range(uint64_t root, vm_space_t *s, uint64_t addr, uint64_t len) {
    if (addr & (PAGE_SIZE - 1)) return -1;
    uint64_t end = (addr + len + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (end <= addr) return -1;
    for (vm_region_t **p = &s->regions; *p;) {
        vm_region_t *r = *p;
        if (r->end <= addr || r->start >= end) {
            p = &r->next;
            continue;
        }
        uint64_t lo = r->start > addr ? r->start : addr, hi = r->end < end ? r->end : end;
        drop_range(root, lo, hi);
        if (lo == r->start && hi == r->end) {                 /* all of it */
            *p = r->next;
            obj_put(r->obj);
            kfree(r);
            continue;
        }
        if (lo == r->start) {                                 /* its start */
            r->offset += hi - r->start;
            r->start = hi;
        } else if (hi == r->end) {                            /* its end */
            r->end = lo;
        } else {                                              /* the middle: two regions */
            vm_region_t *tail = vm_add(s, hi, r->end, r->prot, r->kind, r->obj);
            if (!tail) return -1;
            if (r->obj) r->obj->refs++;
            tail->offset = r->offset + (hi - r->start);
            tail->limit = r->limit;
            r->end = lo;
        }
        p = &r->next;
    }
    return 0;
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

vm_object_t *vm_shm_find(const char *name) {
    for (vm_object_t *o = shm_list; o; o = o->next_shm)
        if (!strcmp(o->name, name)) {
            o->refs++;
            return o;
        }
    return 0;
}

vm_object_t *vm_shm_new(const char *name, uint64_t size) {
    vm_object_t *o = kmalloc(sizeof(*o));
    if (!o) return 0;
    memset(o, 0, sizeof(*o));
    o->kind = VM_SHM;
    o->refs = 1;
    if (vm_shm_resize(o, size) != 0) {
        kfree(o);
        return 0;
    }
    if (name && name[0]) {
        kstrncpy(o->name, name, sizeof(o->name));
        o->next_shm = shm_list;
        shm_list = o;
    }
    return o;
}

int vm_shm_resize(vm_object_t *o, uint64_t size) {
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    if (size > (1ULL << 32)) return -1;
    uint64_t old = o->size / PAGE_SIZE, now = size / PAGE_SIZE;
    if (now == old && o->pages) return 0;
    uint64_t *pages = kmalloc((now ? now : 1) * sizeof(uint64_t));
    if (!pages) return -1;
    memset(pages, 0, (now ? now : 1) * sizeof(uint64_t));
    for (uint64_t i = 0; i < old; i++) {
        if (i < now) pages[i] = o->pages[i];
        else if (o->pages[i]) page_free(o->pages[i]);   /* (a mapping keeps its own reference) */
    }
    kfree(o->pages);
    o->pages = pages;
    o->size = size;
    return 0;
}

int64_t vm_shm_copy(vm_object_t *o, uint64_t offset, void *buf, uint64_t len, int write) {
    if (offset >= o->size) return 0;
    if (len > o->size - offset) len = o->size - offset;
    for (uint64_t done = 0; done < len;) {
        uint64_t i = (offset + done) / PAGE_SIZE, in = (offset + done) % PAGE_SIZE;
        uint64_t n = PAGE_SIZE - in < len - done ? PAGE_SIZE - in : len - done;
        if (!o->pages[i] && !(o->pages[i] = new_page(1))) return done ? (int64_t)done : -1;
        uint8_t *page = (uint8_t *)page_address(o->pages[i]) + in;
        if (write) memcpy(page, (const uint8_t *)buf + done, n);
        else memcpy((uint8_t *)buf + done, page, n);
        done += n;
    }
    return (int64_t)len;
}

void vm_shm_unlink(const char *name) {
    for (vm_object_t **p = &shm_list; *p; p = &(*p)->next_shm)
        if (!strcmp((*p)->name, name)) {
            vm_object_t *o = *p;
            *p = o->next_shm;
            o->name[0] = 0;
            return;
        }
}

uint64_t vm_object_size(vm_object_t *o) { return o->size; }
void vm_object_get(vm_object_t *o) { if (o) o->refs++; }
void vm_object_put(vm_object_t *o) { obj_put(o); }

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
        vm_region_t *copy = vm_add(dst, r->start, r->end, r->prot, r->kind, r->obj);
        if (!copy) return -1;
        copy->offset = r->offset;
        copy->limit = r->limit;
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
