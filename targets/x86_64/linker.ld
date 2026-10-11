ENTRY(start)

/* Physical layout (see docs/ARCHITECTURE.md):
 *   0x100000  kernel image: .boot .text .rodata .data .bss (+ boot stack)
 *   0x400000  page-table pool (paging.c), then the kernel heap from 0x600000
 *             (HEAP_LOW in src/impl/kernel/main.c)
 *
 * kernel_start / kernel_end are consumed by kernel_main(), which maps exactly
 * that range, so the image can grow without touching any other number.
 * The ASSERT below turns "kernel grew into the heap" from a silent triple
 * fault at boot into a build error.
 */
KERNEL_LIMIT = 0x400000;

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .boot :
    {
        KEEP(*(.multiboot_header))
    }

    .text :
    {
        *(.text .text.*)
    }

    .rodata :
    {
        *(.rodata .rodata.*)
    }

    .data :
    {
        *(.data .data.*)
    }

    .bss :
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4096);
    kernel_end = .;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note.GNU-stack)
    }

    ASSERT(kernel_end <= KERNEL_LIMIT,
           "Kernel image has grown into the page-table pool at 0x400000: move the pool and heap addresses in paging.c and kernel_main(), then raise KERNEL_LIMIT")
}
