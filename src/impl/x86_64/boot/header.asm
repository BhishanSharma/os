section .multiboot_header

header_start:
    dd 0xe85250d6
    dd 0
    dd header_end - header_start
    dd 0x100000000 - (0xe85250d6 + 0 + (header_end - header_start))

    ; Framebuffer tag: ask for a linear graphics framebuffer (UEFI machines have
    ; no VGA text mode). Optional: without one the kernel falls back to VGA text.
    align 8
    dw 5        ; type: framebuffer
    dw 1        ; flags: optional
    dd 20       ; size
    dd 0        ; width: no preference
    dd 0        ; height: no preference
    dd 32       ; depth

    ; End tag
    align 8
    dw 0
    dw 0
    dd 8
header_end:
