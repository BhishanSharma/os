/* User programs: static ELF executables loaded at 1 GiB, the start of the
 * user address range (USER_BASE in src/intf/sys/process.h). */
ENTRY(_start)

SECTIONS
{
    . = 0x40000000;

    .text : {
        *(.text._start)
        *(.text .text.*)
    }
    .rodata : {
        *(.rodata .rodata.*)
    }

    . = ALIGN(4096);
    .data : {
        *(.data .data.*)
    }
    .bss : {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
