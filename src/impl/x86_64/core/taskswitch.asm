; Switching between tasks (sys/task.c).
;
; task_switch saves the callee-saved registers on the current kernel stack,
; stores the stack pointer, loads the next task's and pops its registers. A
; task that has never run starts with a stack that "returns" into
; task_start_user (iretq into the program, the frame is already on the stack)
; or task_start_kernel (calls the function left in r12).
bits 64

USER_DATA equ 0x33

section .text

; void task_switch(uint64_t *save_rsp, uint64_t new_rsp)
global task_switch
task_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret

extern bkl_user_entry
global task_start_user
task_start_user:
    sub rsp, 8              ; (16-byte alignment for the call)
    call bkl_user_entry     ; leaving the kernel: let other cores in
    add rsp, 8
    mov ax, USER_DATA
    mov ds, ax
    mov es, ax
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
    iretq                   ; rip, cs, rflags, rsp, ss prepared by task_create_user

extern task_exit_kernel
global task_start_kernel
task_start_kernel:
    sti
    call r12
    call task_exit_kernel   ; does not return

section .note.GNU-stack noalloc noexec nowrite progbits
