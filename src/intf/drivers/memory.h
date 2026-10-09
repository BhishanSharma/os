// src/intf/memory.h
#ifndef MEMORY_H
#define MEMORY_H

#include <stdint.h>

#define PAGE_SIZE 4096  // 4 KB

/* Read the bootloader's memory map (call early, before the multiboot info can
 * be overwritten). Returns the number of usable RAM regions, -1 if none. */
int memory_init(void);

/* 1 if [start, end) lies entirely in usable RAM. */
int memory_range_usable(uint64_t start, uint64_t end);

/* Largest usable RAM range inside [low, high), 2 MiB aligned, at most max_size
 * bytes. Returns 0 and fills start/size, or -1 if there is none. */
int memory_pick_heap(uint64_t low, uint64_t high, uint64_t max_size,
                     uint64_t* start, uint64_t* size);

uint64_t get_total_memory();   // usable RAM in bytes, from the memory map

/* One 4 KiB page from the heap (the `alloc` debug command). */
void* alloc_frame();
void free_frame(void* frame);

#endif
