/*
 * start_arm64_aapcs.S : freestanding AArch64 _start and syscall wrappers for
 * skj-cc-arm64 under the AAPCS64 calling convention (cc P4).
 *
 * AAPCS64 passes function args in x0..x7; the AArch64 Linux syscall ABI passes
 * args in x0..x5 with the number in x8.  For read/write/exit those are the
 * same registers, so each wrapper just sets x8 and issues svc.  _start switches
 * to a low-memory stack (matching the runtime's arena placement) and calls
 * main, then passes main's return value to exit_group.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */
	.text
	.globl _start
	.globl write
	.globl read
	.globl exit
	.globl _exit

_start:
	ldr	x0, =stack_top
	mov	sp, x0			/* switch to the low-memory stack */
	bl	main
	/* main's return value is in w0; pass it to exit_group */
	mov	x8, #93
	svc	#0
1:	b	1b

/* ssize_t write(int fd, const void *buf, size_t n); AAPCS64 args already in
   x0/x1/x2, which are the syscall arg registers too */
write:
	mov	x8, #64
	svc	#0
	ret

/* ssize_t read(int fd, void *buf, size_t n) */
read:
	mov	x8, #63
	svc	#0
	ret

/* void exit(int status); status already in x0 */
exit:
_exit:
	mov	x8, #93
	svc	#0
1:	b	1b

	.bss
	.balign	16
	.space	131072
stack_top:
