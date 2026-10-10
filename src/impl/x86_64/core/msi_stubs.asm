; msi_stubs.asm - entry points for message-signalled interrupts (vectors 0x50-0x6F)
;
; Each stub saves the registers, calls msi_dispatch(slot) (which runs the
; device's handler and acknowledges the local APIC), and returns.

extern msi_dispatch

%macro MSI_STUB 1
global msi_stub_%1
msi_stub_%1:
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
    mov rdi, %1
    mov rbx, rsp               ; keep the stack 16-byte aligned for the C ABI
    and rsp, -16
    call msi_dispatch
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
%endmacro

%assign i 0
%rep 32
MSI_STUB i
%assign i i+1
%endrep

section .rodata
global msi_stub_table
msi_stub_table:
%assign i 0
%rep 32
    dq msi_stub_ %+ i
%assign i i+1
%endrep
