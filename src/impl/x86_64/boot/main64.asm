global long_mode_start
extern kernel_main

section .text
bits 64
long_mode_start:
    ; Enable x87/SSE state before kernel code or libraries use SIMD instructions.
    mov rax, cr0
    and rax, ~((1 << 2) | (1 << 3))
    or rax, (1 << 1)
    mov cr0, rax

    mov rax, cr4
    or rax, (1 << 9) | (1 << 10)
    mov cr4, rax
    fninit

    mov ax, 0
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    ; mov dword [0xb8000], 0x2f4b2f4f
    call kernel_main
    
    hlt