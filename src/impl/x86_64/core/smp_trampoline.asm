; smp_trampoline.asm - how another core gets from reset to the kernel
;
; sys/smp.c copies trampoline_start..trampoline_end to physical 0x8000 and
; fills the parameters at its end, then sends the core STARTUP with page 8:
; it begins here in 16-bit real mode (CS = 0x0800). It loads a small GDT of
; its own, enters protected mode, turns on PAE, loads the kernel's page
; tables, enables long mode and paging, and calls ap_main(cpu) on its stack.

TRAMPOLINE equ 0x8000
%define T(x) (TRAMPOLINE + (x) - trampoline_start)

section .rodata
align 16
global trampoline_start
global trampoline_end
global trampoline_params

bits 16
trampoline_start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    o32 lgdt [T(tgdt_ptr)]
    mov eax, cr0
    or eax, 1                           ; protected mode
    mov cr0, eax
    jmp dword 0x08:T(pm32)

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov eax, cr4
    or eax, (1 << 5) | (1 << 9) | (1 << 10)    ; PAE, SSE (OSFXSR, OSXMMEXCPT)
    mov cr4, eax
    mov eax, [T(tp_cr3)]
    mov cr3, eax
    mov ecx, 0xC0000080                 ; EFER
    rdmsr
    or eax, 1 << 8                      ; long mode
    wrmsr
    mov eax, cr0
    and eax, ~((1 << 2) | (1 << 3))     ; no FPU emulation, no task-switched trap
    or eax, (1 << 31) | (1 << 1)        ; paging, monitor coprocessor
    mov cr0, eax
    jmp 0x18:T(lm64)

bits 64
lm64:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov rsp, [T(tp_stack)]
    mov rdi, [T(tp_cpu)]
    mov rax, [T(tp_entry)]
    call rax
.halt:
    cli
    hlt
    jmp .halt

align 8
tgdt:
    dq 0
    dq 0x00CF9A000000FFFF               ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF               ; 0x10: data
    dq 0x00209A0000000000               ; 0x18: 64-bit code
    dq 0x0000920000000000               ; 0x20: data
tgdt_ptr:
    dw tgdt_ptr - tgdt - 1
    dd T(tgdt)

align 8
trampoline_params:
tp_cr3:   dq 0
tp_stack: dq 0
tp_entry: dq 0
tp_cpu:   dq 0
trampoline_end:

; ---- The other cores' timer (vector 0x41) -----------------------------------

section .text
bits 64
extern ap_timer_interrupt

global ap_timer_stub
ap_timer_stub:
    push rax
    push rcx
    push rdx
    push rbx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov rdi, [rsp + 15 * 8 + 8]         ; the interrupted CS (RPL 3: a user program)
    mov rbx, rsp
    and rsp, -16
    call ap_timer_interrupt
    mov rsp, rbx
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rbx
    pop rdx
    pop rcx
    pop rax
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
