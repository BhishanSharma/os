; Entering and leaving user mode (ring 3), and the system call entry point.
;
; user_enter() saves the kernel's callee-saved registers and stack pointer,
; then `iretq`s into the program. The program comes back only through
; user_return(), called by the exit system call, a fault, or Ctrl+C: it
; switches back to the saved stack and returns from user_enter() with the
; exit code. While the program runs, interrupts and system calls arrive on
; the stack in TSS.rsp0 (see process.c).
bits 64

USER_CODE equ 0x2B          ; GDT_USER_CODE | RPL 3
USER_DATA equ 0x33          ; GDT_USER_DATA | RPL 3
KERNEL_DATA equ 0x10

extern syscall_dispatch

section .bss
align 8
saved_kernel_rsp: resq 1

section .text

; int64_t user_enter(uint64_t entry, uint64_t user_rsp)
global user_enter
user_enter:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rel saved_kernel_rsp], rsp

    mov ax, USER_DATA
    mov ds, ax
    mov es, ax

    push USER_DATA          ; ss
    push rsi                ; rsp
    push 0x202              ; rflags: IF set, reserved bit 1
    push USER_CODE          ; cs
    push rdi                ; rip

    ; Leave no kernel values in registers.
    xor eax, eax
    xor ebx, ebx
    xor ecx, ecx
    xor edx, edx
    xor esi, esi
    xor edi, edi
    xor ebp, ebp
    xor r8d, r8d
    xor r9d, r9d
    xor r10d, r10d
    xor r11d, r11d
    xor r12d, r12d
    xor r13d, r13d
    xor r14d, r14d
    xor r15d, r15d
    iretq

; void user_return(int64_t code) -- never returns to its caller
global user_return
user_return:
    cli
    mov rax, rdi
    mov rsp, [rel saved_kernel_rsp]
    mov cx, KERNEL_DATA
    mov ds, cx
    mov es, cx
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

; int 0x80: rax = number, arguments in rdi, rsi, rdx, r10, r8, r9; the result
; goes back in rax. The frame matches struct exc_frame (core/exceptions.h).
global syscall_stub
syscall_stub:
    push qword 0            ; error code slot
    push qword 0x80         ; vector
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cld
    mov rdi, rsp            ; struct exc_frame*
    mov rbp, rsp
    and rsp, -16
    call syscall_dispatch   ; stores the result in frame->rax
    mov rsp, rbp

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

section .note.GNU-stack noalloc noexec nowrite progbits
