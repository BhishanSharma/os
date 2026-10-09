; The system call entry point. Programs enter ring 3 through task_start_user
; (taskswitch.asm); while one runs, interrupts and system calls arrive on its
; kernel stack (TSS.rsp0, set by the scheduler).
bits 64


extern syscall_dispatch

section .text

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
