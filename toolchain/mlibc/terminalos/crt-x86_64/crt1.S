.section .text
.global _start
_start:
	xor %rbp, %rbp
	lea main(%rip), %rdi
	mov %rsp, %rsi
	call __mlibc_entry
	ud2
.section .note.GNU-stack,"",%progbits
