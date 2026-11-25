/* SPDX-License-Identifier: GPL-2.0-only */
/* i386 glibc startup without depending on a host multilib CRT/libgcc. */
.text
.global _start
.type _start,@function
_start:
	xor %ebp,%ebp
	pop %esi
	mov %esp,%ecx
	and $-16,%esp
	push %eax
	push %esp
	push %edx
	push $0
	push $0
	push %ecx
	push %esi
	push $main
	call __libc_start_main@PLT
	hlt
.section .note.GNU-stack,"",@progbits
