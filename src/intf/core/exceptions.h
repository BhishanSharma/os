#ifndef EXCEPTIONS_H
#define EXCEPTIONS_H

#include <stdint.h>

// Register state saved by core/exc_stubs.asm (layout must match the stubs).
struct exc_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error;              // pushed by the stub / CPU
    uint64_t rip, cs, rflags, rsp, ss;   // pushed by the CPU
};

// Install gates for vectors 0-31 (called from idt_init()).
void exceptions_init(void);

// Called from assembly. Fatal exceptions never return; #DB and #BP do.
void exception_handler(struct exc_frame* frame);

// Stop the machine with a red panic screen (also sent to COM1). Use it for
// "this can't happen" conditions in kernel code.
void kpanic(const char* message) __attribute__((noreturn));

#endif
