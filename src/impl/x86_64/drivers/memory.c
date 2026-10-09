#include "drivers/memory.h"
#include "drivers/heap.h"
#include "core/multiboot2.h"

/* Usable RAM as reported by the bootloader's memory map. */
#define MAX_REGIONS 32
static mb2_region_t regions[MAX_REGIONS];
static int region_count = -1;            /* -1: no memory map */
static uint64_t total_usable;

void* memset(void* ptr, int value, uint64_t num) {
    uint8_t* p = (uint8_t*)ptr;
    for (uint64_t i = 0; i < num; i++) {
        p[i] = (uint8_t)value;
    }
    return ptr;
}

int memory_init(void) {
    region_count = mb2_get_memory_map(regions, MAX_REGIONS);
    total_usable = 0;
    for (int i = 0; i < region_count; i++) total_usable += regions[i].length;
    return region_count;
}

void memory_reserve(uint64_t start, uint64_t end) {
    for (int i = 0; i < region_count; i++) {
        uint64_t s = regions[i].base, e = s + regions[i].length;
        if (end <= s || start >= e) continue;
        if (start > s && end < e && region_count < MAX_REGIONS) {
            // Split: keep [s, start) here, append [end, e).
            regions[region_count].base = end;
            regions[region_count].length = e - end;
            region_count++;
            regions[i].length = start - s;
        } else if (start > s) {
            regions[i].length = start - s;
        } else if (end < e) {
            regions[i].base = end;
            regions[i].length = e - end;
        } else {
            regions[i].length = 0;
        }
    }
}

int memory_range_usable(uint64_t start, uint64_t end) {
    for (int i = 0; i < region_count; i++)
        if (start >= regions[i].base && end <= regions[i].base + regions[i].length) return 1;
    return 0;
}

int memory_pick_heap(uint64_t low, uint64_t high, uint64_t max_size,
                     uint64_t* start, uint64_t* size) {
    const uint64_t align = 2ULL * 1024 * 1024;   /* heap is mapped with 2 MiB pages */
    uint64_t best_start = 0, best_size = 0;
    for (int i = 0; i < region_count; i++) {
        uint64_t s = regions[i].base, e = regions[i].base + regions[i].length;
        if (s < low) s = low;
        if (e > high) e = high;
        s = (s + align - 1) & ~(align - 1);
        e &= ~(align - 1);
        if (e > s && e - s > best_size) { best_start = s; best_size = e - s; }
    }
    if (!best_size) return -1;
    *start = best_start;
    *size = best_size > max_size ? max_size : best_size;
    return 0;
}

uint64_t get_total_memory() {
    return total_usable;
}

/* Page frames come from the heap (which is identity mapped): over-allocate,
 * align to 4 KiB and keep the original pointer just below the frame. */
void* alloc_frame() {
    uint8_t* raw = kmalloc(2 * PAGE_SIZE);
    if (!raw) return 0;
    uint64_t frame = ((uint64_t)raw + sizeof(void*) + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
    ((void**)frame)[-1] = raw;
    return (void*)frame;
}

void free_frame(void* frame) {
    if (frame) kfree(((void**)frame)[-1]);
}
