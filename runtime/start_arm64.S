/* start_arm64.S - minimal AArch64 (ARM64) runtime for Linux user-mode.
 *
 * Provides _start (the ELF entry point) and thin wrappers for the syscalls
 * the test programs need: write(2), read(2) and exit(2).  qemu-aarch64 uses
 * the standard Linux/AArch64 syscall convention:
 *
 *   x8 = syscall number   (write=64, read=63, exit=93)
 *   x0..x5 = args
 *   svc #0 to invoke
 *   return value in x0
 *
 * Our calling convention passes all args on the stack (not in registers), so
 * the wrappers load from the stack before issuing svc.
 *
 * The backend uses an ILP32-style address model: every address is a 32-bit
 * value.  For that to hold, _start switches from the kernel-provided
 * (high) stack to a low-memory stack allocated here in .bss, and the bump
 * heap lives in .bss too.  A non-PIE executable links these low (< 4GB), so
 * every live address fits in 32 bits.
 *
 * _start drops argc/argv (tests take no arguments), calls main, then passes
 * main's return value to exit.
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

/* int write(int fd, const char *buf, int n); args on stack */
write:
	ldr	w0, [sp, #0]		/* fd */
	ldr	w1, [sp, #8]		/* buf (32-bit addr, zero-extended) */
	ldr	w2, [sp, #16]		/* n */
	mov	x8, #64
	svc	#0
	ret

/* int read(int fd, char *buf, int n); args on stack */
read:
	ldr	w0, [sp, #0]
	ldr	w1, [sp, #8]
	ldr	w2, [sp, #16]
	mov	x8, #63
	svc	#0
	ret

/* void exit(int status); arg on stack */
exit:
_exit:
	ldr	w0, [sp, #0]
	mov	x8, #93
	svc	#0
1:	b	1b

/* void *__moo_arena_alloc(int size);
 *   Bump allocator.  Aligns size up to 8 bytes.  Returns pointer in x0. */
	.globl __moo_arena_alloc
__moo_arena_alloc:
	ldr	w0, [sp, #0]
	add	w0, w0, #7
	bic	w0, w0, #7		/* round up to 8 */
	ldr	x1, =__moo_arena_ptr
	ldr	w2, [x1]		/* old pointer (return value) */
	add	w3, w2, w0		/* new pointer */
	str	w3, [x1]
	mov	w0, w2
	ret

/* void __moo_arena_reset(void); */
	.globl __moo_arena_reset
__moo_arena_reset:
	ldr	x1, =__moo_arena_ptr
	ldr	x2, =__moo_arena
	str	w2, [x1]
	ret

/* __cont_capture()
 *   Called by IR_CAPTURE inline code after pushing x19-x28.  The bl return
 *   address (x30) is saved on the stack as part of the captured segment.
 *
 *   Reads the 12-byte mark slot via __cont_mark_sp (fp, sp, re-entry PC as
 *   32-bit words), bump-allocates a buffer in __cont_arena, copies the stack
 *   segment [capture_sp, mark_sp) into it, then restores the mark's fp/sp and
 *   branches to the mark's re-entry PC with x0 = buffer address (nonzero).
 *
 *   Clobbers all regs (callee-saves are inside the captured segment). */
	.globl __cont_capture
__cont_capture:
	sub	x0, sp, #16
	mov	sp, x0
	str	x30, [sp, #0]		/* save return address (part of segment) */
	mov	x11, sp			/* x11 = capture_sp */
	mov	x12, x29		/* x12 = capture_fp */

	/* read the 12-byte mark slot (32-bit words, zero-extended) */
	ldr	x9, =__cont_mark_sp
	ldr	w9, [x9]		/* x9 = mark slot pointer */
	ldr	w13, [x9, #0]		/* x13 = mark's saved_fp */
	ldr	w14, [x9, #4]		/* x14 = mark's saved_sp */
	ldr	w15, [x9, #8]		/* x15 = mark's re-entry PC */

	sub	x2, x14, x11		/* x2 = size = mark_sp - capture_sp */

	/* bump-allocate: 12-byte header + size bytes */
	ldr	x9, =__cont_arena_ptr
	ldr	w0, [x9]		/* x0 = buf */
	str	w11, [x0, #0]		/* buf[0] = capture_sp */
	str	w12, [x0, #4]		/* buf[1] = capture_fp */
	str	w2, [x0, #8]		/* buf[2] = size */
	add	x10, x0, x2
	add	x10, x10, #12
	str	w10, [x9]		/* advance arena pointer */

	/* copy stack segment (4-byte words): capture_sp -> buf+12 */
	add	x3, x0, #12		/* dst */
	mov	x4, x11			/* src */
	mov	x5, x2			/* remaining bytes */
	cbz	x5, .Lcap_done
.Lcap_loop:
	ldr	w10, [x4]
	str	w10, [x3]
	add	x4, x4, #4
	add	x3, x3, #4
	sub	x5, x5, #4
	cbnz	x5, .Lcap_loop
.Lcap_done:
	mov	x29, x13		/* restore mark's fp */
	mov	sp, x14			/* restore mark's sp */
	br	x15			/* jump to re-entry (x0 = buf, nonzero) */

/* __cont_resume(buf, value)
 *   Restores the captured stack segment and returns into it.  The saved x30
 *   on the stack (from __cont_capture) is restored by the tail here, landing
 *   at the IR_CAPTURE resume point with x0 = value.
 *
 *   Args on stack: [sp, #0] = buf, [sp, #8] = value */
	.globl __cont_resume
__cont_resume:
	ldr	w3, [sp, #0]		/* x3 = buf */
	ldr	w1, [sp, #8]		/* x1 = resume value */

	ldr	w11, [x3, #0]		/* capture_sp */
	ldr	w12, [x3, #4]		/* capture_fp */
	ldr	w2, [x3, #8]		/* size */

	mov	x29, x12		/* restore fp */
	mov	sp, x11			/* restore sp = capture_sp */

	/* copy segment back: buf+12 -> capture_sp */
	add	x4, x3, #12		/* src */
	mov	x5, sp			/* dst */
	cbz	x2, .Lres_done
.Lres_loop:
	ldr	w10, [x4]
	str	w10, [x5]
	add	x4, x4, #4
	add	x5, x5, #4
	sub	x2, x2, #4
	cbnz	x2, .Lres_loop
.Lres_done:
	mov	w0, w1			/* x0 = resume value */
	ldr	x30, [sp, #0]		/* restore x30 (saved by __cont_capture) */
	add	sp, sp, #16
	ret				/* return to IR_CAPTURE resume point */

	.data
	.balign	8
__moo_arena_ptr:
	.word	__moo_arena
	.globl	__cont_mark_sp
__cont_mark_sp:
	.word	0
	.globl	__cont_arena_ptr
__cont_arena_ptr:
	.word	__cont_arena

	.bss
	.balign	16
__moo_arena:
	.space	131072
	.balign	16
__cont_arena:
	.space	131072
	.balign	16
	.space	131072
stack_top:
