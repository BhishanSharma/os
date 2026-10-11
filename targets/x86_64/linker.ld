ENTRY(start)

/* Physical layout (see docs/ARCHITECTURE.md):
 *   0x100000  kernel image: .boot, then .text .rodata .data .bss (+ boot stack)
 *   0x400000  page-table pool (paging.c), then the kernel heap from 0x600000
 *             (HEAP_LOW in src/impl/kernel/main.c)
 *
 * Virtual layout: .boot (the multiboot header and the code that turns on
 * paging) runs where it is loaded; everything else runs in the top 2 GiB, at
 * KERNEL_VMA + its physical address (src/intf/drivers/addr.h). Boot loaders
 * place the segments by their physical (load) addresses.
 *
 * kernel_phys_end is consumed by kernel_main(), which maps exactly the image.
 * The ASSERT turns "kernel grew into the page tables" from a silent triple
 * fault at boot into a build error.
 */
KERNEL_VMA   = 0xFFFFFFFF80000000;
KERNEL_LIMIT = 0x400000;

SECTIONS
{
    . = 1M;
    kernel_phys_start = .;

    .boot :
    {
        KEEP(*(.multiboot_header))
        *(.boot.text)
        *(.boot.rodata)
    }

    . = ALIGN(4096) + KERNEL_VMA;
    kernel_start = .;

    .text : AT(ADDR(.text) - KERNEL_VMA)
    {
        *(.text .text.*)
    }

    .rodata : AT(ADDR(.rodata) - KERNEL_VMA)
    {
        *(.rodata .rodata.*)
    }

    .data : AT(ADDR(.data) - KERNEL_VMA)
    {
        *(.data .data.*)
    }

    .bss : AT(ADDR(.bss) - KERNEL_VMA)
    {
        *(COMMON)
        *(.bss .bss.*)
    }

    . = ALIGN(4096);
    kernel_end = .;
    kernel_phys_end = . - KERNEL_VMA;

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.comment)
        *(.note.GNU-stack)
    }

    ASSERT(kernel_phys_end <= KERNEL_LIMIT,
           "Kernel image has grown into the page-table pool at 0x400000: move the pool and heap addresses in paging.c and kernel_main(), then raise KERNEL_LIMIT")
}
