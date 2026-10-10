extern isr_keyboard

global irq1_stub
irq1_stub:
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
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

    sub rsp, 8          ; keep rsp 16-byte aligned for the C ABI
    call isr_keyboard
    add rsp, 8

    ; send EOI to PIC
    mov al, 0x20
    out 0x20, al

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
    pop rsp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    iretq

extern isr_timer       ; Your C handler for timer interrupt
extern user_check_interrupt

global irq0_stub
irq0_stub:
    ; Save all general-purpose registers
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
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

    ; Call C handler
    sub rsp, 8          ; keep rsp 16-byte aligned for the C ABI
    call isr_timer
    add rsp, 8

    ; Send End-of-Interrupt (EOI) to PIC
    mov al, 0x20
    out 0x20, al

    ; Interrupted a user program (CS in the iret frame, above the 16 saved
    ; registers, has RPL 3)? Let process.c end it if Ctrl+C was pressed; that
    ; call does not return then.
    test qword [rsp + 16*8 + 8], 3
    jz .resume
    sub rsp, 8
    call user_check_interrupt
    add rsp, 8
.resume:

    ; Restore all registers
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
    pop rsp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    iretq

extern nic_handle_irq

global irq_nic_stub
irq_nic_stub:
    push rax
    push rcx
    push rdx
    push rbx
    push rsp
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

    sub rsp, 8          ; keep rsp 16-byte aligned for the C ABI
    call nic_handle_irq
    add rsp, 8

    ; The NIC usually sits on the slave PIC (IRQ 8-15): EOI both PICs.
    ; (An extra EOI to an idle slave is harmless.)
    mov al, 0x20
    out 0xA0, al
    out 0x20, al

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
    pop rsp
    pop rbx
    pop rdx
    pop rcx
    pop rax

    iretq

; Spurious interrupts from the 8259 PICs. When a device drops its interrupt
; line between the PIC raising it and the CPU acknowledging it (the e1000
; does when the timer poll clears its interrupt cause first), the PIC
; delivers its lowest-priority vector instead: IRQ 7 (master) or IRQ 15
; (slave). Lines 7 and 15 are masked, so these are always spurious.
global irq_spurious_master
irq_spurious_master:
    iretq                   ; no EOI: the master has no interrupt in service

global irq_spurious_slave
irq_spurious_slave:
    push rax
    mov al, 0x20
    out 0x20, al            ; EOI to the master only (for the cascade line)
    pop rax
    iretq
