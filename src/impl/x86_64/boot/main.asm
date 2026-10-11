; main.asm - from the boot loader to kernel_main
;
; This code runs at its load address (section .boot, linked low); the rest of
; the kernel runs at KERNEL_VMA + its load address (targets/x86_64/linker.ld).
; Before paging is on, kernel symbols are reached by their physical address,
; PHYS(symbol). The boot page tables map:
;   the first 4 GiB at 0                  (this code; the multiboot information)
;   the first 4 GiB at DIRECT_MAP         (phys_to_virt() until paging_init)
;   the first 1 GiB at KERNEL_VMA         (the kernel image)
; paging_init() replaces them with the kernel's real tables.

global start
global start64
extern long_mode_start

KERNEL_VMA equ 0xFFFFFFFF80000000
%define PHYS(x) ((x) - KERNEL_VMA)

section .boot.text progbits alloc exec nowrite
    bits 32
start:
    mov esp, PHYS(stack_top)
    mov [PHYS(multiboot_info)], ebx   ; multiboot2 info (framebuffer etc.), read by kernel_main

    call check_multiboot
    call ckeck_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging

    lgdt [boot_gdt.pointer]
    jmp boot_gdt.code_segment:enter_high

    hlt

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret

.no_multiboot:
    mov al, "M"
    jmp error

; 2 MiB pages for the first 4 GiB (four page directories), reached from PML4
; entry 0 (identity) and entry 256 (the direct map); the first directory
; also from PML4 entry 511, PDPT entry 510 (KERNEL_VMA).
setup_page_tables:
    mov eax, PHYS(page_table_l3)
    or eax, 0b11
    mov [PHYS(page_table_l4)], eax
    mov [PHYS(page_table_l4) + 256 * 8], eax
    mov eax, PHYS(page_table_l3_high)
    or eax, 0b11
    mov [PHYS(page_table_l4) + 511 * 8], eax
    mov eax, PHYS(page_table_l2)
    or eax, 0b11
    mov [PHYS(page_table_l3_high) + 510 * 8], eax

    mov ecx, 0
.l3_loop:
    mov eax, ecx
    shl eax, 12                 ; page_table_l2 + ecx * 4096
    add eax, PHYS(page_table_l2)
    or eax, 0b11
    mov [PHYS(page_table_l3) + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .l3_loop

    mov ecx, 0
.loop:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011
    mov [PHYS(page_table_l2) + ecx * 8], eax

    inc ecx
    cmp ecx, 2048
    jne .loop
    ret

enable_paging:
    mov eax, PHYS(page_table_l4)
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ret

; ---- From Terminal OS's UEFI loader (loader/main.c) ------------------------
; Already in long mode, on the loader's identity page tables, interrupts off;
; edi = the multiboot2 information (below 4 GiB). Same state as after
; enable_paging + lgdt above, then on to the same 64-bit start.
    bits 64
; NASM warns that PHYS(x) does not fit a signed 32-bit displacement: the
; linker's result (x - KERNEL_VMA, below 16 MiB) does.
[warning -number-overflow]
start64:
    cli
    mov rsp, PHYS(stack_top)
    mov [PHYS(multiboot_info)], edi

    ; The same tables as setup_page_tables.
    mov rax, PHYS(page_table_l3)
    or rax, 0b11
    mov [PHYS(page_table_l4)], rax
    mov [PHYS(page_table_l4) + 256 * 8], rax
    mov rax, PHYS(page_table_l3_high)
    or rax, 0b11
    mov [PHYS(page_table_l4) + 511 * 8], rax
    mov rax, PHYS(page_table_l2)
    or rax, 0b11
    mov [PHYS(page_table_l3_high) + 510 * 8], rax
    xor ecx, ecx
.l3:
    mov rax, rcx
    shl rax, 12
    add rax, PHYS(page_table_l2)
    or rax, 0b11
    mov [PHYS(page_table_l3) + rcx * 8], rax
    inc ecx
    cmp ecx, 4
    jne .l3
    xor ecx, ecx
.l2:
    mov rax, rcx
    shl rax, 21
    or rax, 0b10000011
    mov [PHYS(page_table_l2) + rcx * 8], rax
    inc ecx
    cmp ecx, 2048
    jne .l2
    mov rax, PHYS(page_table_l4)
    mov cr3, rax

    lgdt [boot_gdt.pointer]
    push boot_gdt.code_segment
    lea rax, [rel enter_high]
    push rax
    retfq

[warning *number-overflow]

; Both ways in: long mode, the boot tables loaded. Move the stack to the
; kernel's addresses and jump there.
enter_high:
    mov rax, KERNEL_VMA
    add rsp, rax
    mov rax, long_mode_start
    jmp rax

    bits 32
error:
    mov dword [0xb8000], 0x4f524f45
    mov dword [0xb8004], 0x4f3a4f52
    mov dword [0xb8008], 0x4f204f20
    mov byte [0xb800a], al
    hlt

ckeck_cpuid:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    je .no_cpuid
    ret

.no_cpuid:
    mov al, "C"
    jmp error

check_long_mode:
    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode

    ret

.no_long_mode:
    mov al, "L"
    jmp error

; The boot GDT: at its load address, for lgdt in 32-bit mode. gdt_init()
; loads the kernel's own.
section .boot.rodata progbits alloc noexec nowrite
align 8
boot_gdt:
    dq 0
.code_segment: equ $ - boot_gdt
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - boot_gdt - 1
    dq boot_gdt

section .data
global multiboot_info
multiboot_info:                 ; physical address (phys_to_virt to read it)
    dd 0

section .bss
    align 4096
page_table_l4:
    resb 4096
page_table_l3:
    resb 4096
page_table_l3_high:
    resb 4096
page_table_l2:
    resb 4096 * 4
; One page that paging_init() leaves unmapped: running off the bottom of the
; stack then raises a page fault (-> double fault -> panic screen) instead of
; silently overwriting whatever lies below.
global stack_guard
stack_guard:
    resb 4096
stack_bottom:
    resb 4096 * 8               ; 32 KiB boot stack
stack_top:

section .note.GNU-stack noalloc noexec nowrite progbits
