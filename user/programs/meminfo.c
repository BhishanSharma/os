// meminfo - how the memory is used: the kernel heap and the pages for programs
#include <stdio.h>
#include <os.h>

int main(void) {
    struct os_meminfo m;
    if (meminfo(&m) != 0) {
        printf("meminfo: not available\n");
        return 1;
    }
    printf("=== Memory ===\n");
    printf("RAM:          %lu MiB usable\n", m.ram >> 20);
    printf("Kernel heap:  %lu KiB used of %lu MiB (%lu allocations)\n",
           m.heap_used >> 10, m.heap_total >> 20, m.heap_blocks);
    printf("Programs:     %lu of %lu MiB in use (pages from the rest of RAM)\n",
           m.pages_used >> 20, m.pages_total >> 20);
    return 0;
}
