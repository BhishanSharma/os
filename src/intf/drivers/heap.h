// heap.h
#ifndef HEAP_H
#define HEAP_H

#include <stdint.h>

void heap_init(uint64_t start, uint64_t size);
void* kmalloc(uint64_t size);
void kfree(void* ptr);

// New functions for memory info
uint64_t heap_get_used();
uint64_t heap_get_free();
uint64_t heap_get_total();
uint64_t heap_get_allocations();

#endif