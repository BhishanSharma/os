; Program entry point. The kernel starts us with the stack laid out as the
; SysV ABI describes: [rsp] = argc, then argv[0..argc-1], then NULL.
bits 64
extern main
extern exit

section .text._start
global _start
_start:
    mov rdi, [rsp]          ; argc
    lea rsi, [rsp + 8]      ; argv
    and rsp, -16            ; the ABI's alignment for the call (SSE code relies on it)
    call main
    mov edi, eax
    call exit               ; never returns
.hang:
    jmp .hang

section .note.GNU-stack noalloc noexec nowrite progbits
