// A new thread starts here (Sysdeps<Clone>) with rdi = the stack that
// PrepareStack laid out: entry, tcb, arg, then padding.
.section .text
.global __mlibc_thread_entry
__mlibc_thread_entry:
	mov %rdi, %rsp
	pop %rdi
	pop %rsi
	pop %rdx
	add $8, %rsp
	xor %rbp, %rbp
	call __mlibc_thread_trampoline
	ud2
.section .note.GNU-stack,"",%progbits
