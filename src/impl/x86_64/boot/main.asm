global start
extern long_mode_start

section .text
    bits 32
start:
    mov esp, stack_top
    mov [multiboot_info], ebx   ; multiboot2 info (framebuffer etc.), read by kernel_main

    call check_multiboot
    call ckeck_cpuid
    call check_long_mode

    call setup_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code_segment:long_mode_start

    hlt

check_multiboot:
    cmp eax, 0x36d76289
    jne .no_multiboot
    ret

.no_multiboot:
    mov al, "M"
    jmp error

; Identity-map the first 4 GiB with 2 MiB pages (four page directories), so the
; multiboot2 info and a UEFI framebuffer below 4 GiB are reachable before
; paging_init() builds the real tables.
setup_page_tables:
    mov eax, page_table_l3
    or eax, 0b11
    mov [page_table_l4], eax

    mov ecx, 0
.l3_loop:
    mov eax, ecx
    shl eax, 12                 ; page_table_l2 + ecx * 4096
    add eax, page_table_l2
    or eax, 0b11
    mov [page_table_l3 + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .l3_loop

    mov ecx, 0
.loop:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011
    mov [page_table_l2 + ecx * 8], eax

    inc ecx
    cmp ecx, 2048
    jne .loop
    ret
    
enable_paging:
    mov eax, page_table_l4
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
global start64
start64:
    cli
    mov rsp, stack_top
    mov [multiboot_info], edi

    ; The same identity map of the first 4 GiB as setup_page_tables.
    mov rax, page_table_l3
    or rax, 0b11
    mov [page_table_l4], rax
    xor ecx, ecx
.l3:
    mov rax, rcx
    shl rax, 12
    add rax, page_table_l2
    or rax, 0b11
    mov [page_table_l3 + rcx * 8], rax
    inc ecx
    cmp ecx, 4
    jne .l3
    xor ecx, ecx
.l2:
    mov rax, rcx
    shl rax, 21
    or rax, 0b10000011
    mov [page_table_l2 + rcx * 8], rax
    inc ecx
    cmp ecx, 2048
    jne .l2
    mov rax, page_table_l4
    mov cr3, rax

    lgdt [gdt64.pointer]
    push gdt64.code_segment
    lea rax, [rel .reload]
    push rax
    retfq
.reload:
    jmp long_mode_start

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

section .data
global multiboot_info
multiboot_info:
    dd 0

section .bss
    align 4096
page_table_l4:
    resb 4096
page_table_l3:
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

section .rodata
gdt64:
    dq 0
.code_segment: equ $ - gdt64
    dq (1 << 43) | (1 << 44) | (1 << 47) | (1 << 53)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64