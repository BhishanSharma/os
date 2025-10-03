global idt_load

idt_load:
    lidt [rdi]   ; rdi = pointer to IDT descriptor
    ret
