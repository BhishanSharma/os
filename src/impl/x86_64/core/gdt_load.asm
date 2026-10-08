bits 64
global gdt_flush

section .text
; void gdt_flush(const void* gdtr)   rdi = pointer to the GDT descriptor
gdt_flush:
    lgdt [rdi]
    push 0x08                   ; kernel code selector
    lea rax, [rel .reload]
    push rax
    o64 retf                    ; far return reloads CS
.reload:
    mov ax, 0x10                ; kernel data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    xor eax, eax
    mov fs, ax
    mov gs, ax
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
