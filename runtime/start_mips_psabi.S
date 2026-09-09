/* start_mips_psabi.S - MIPS I (o32, little-endian) runtime for the
 * platform-ABI tier, as emulated by qemu-mipsel.
 *
 * runtime/start_mips.S serves the toolkit's own stack convention: its
 * write() and helpers read their arguments off the stack, which is what a
 * non-psABI build of the MIPS backend emits.  A tier that links gcc-built C
 * (libexc and its host binding) needs the same services under the o32
 * convention instead, arguments in $a0 upward, and that is this file.  The
 * two are never linked together.
 *
 * The 64-bit helpers are not here: under the platform ABI they are ordinary
 * C, so runtime/soft64.c (or the toolchain's libgcc) supplies them.
 *
 * o32 requires the caller to reserve at least 16 bytes of argument home
 * space, which is where a callee spills its register parameters.  The
 * emitted code does this at every call; the two hand-written callers here
 * (_start and the coroutine body launch) do it explicitly.
 */

	.set reorder

	.text
	.globl _start
	.globl read
	.globl write
	.globl exit
	.globl _exit
	.globl __moo_arena_alloc
	.globl __moo_arena_reset
	.globl __exc_src_next
	.globl __exc_src_yield

/* Linux/MIPS o32 syscall numbers: read 4003, write 4004, exit 4001 */

_start:
	la	$sp, stack_top		/* our own stack, like start_mips.S: the
					   kernel's is one page and the deeper
					   tests overflow it */
	li	$t0, -16
	and	$sp, $sp, $t0
	addiu	$sp, $sp, -16		/* argument home for main */
	jal	main
	move	$a0, $v0
	li	$v0, 4001
	syscall
1:	b	1b

/* int read(int fd, char *buf, int n);   args already in $a0-$a2 (o32) */
read:
	li	$v0, 4003
	syscall
	jr	$ra

/* int write(int fd, const char *buf, int n); */
write:
	li	$v0, 4004
	syscall
	jr	$ra

/* void exit(int status);  (alias _exit) */
exit:
_exit:
	li	$v0, 4001
	syscall
1:	b	1b

/* void *__moo_arena_alloc(int size);  bump allocator, 4-byte aligned.
   size in $a0 (o32), result in $v0. */
__moo_arena_alloc:
	addiu	$a0, $a0, 3
	li	$t1, -4
	and	$a0, $a0, $t1
	la	$t0, __moo_arena_ptr
	lw	$v0, 0($t0)		/* old pointer, the result */
	addu	$t2, $v0, $a0
	sw	$t2, 0($t0)
	jr	$ra

/* void __moo_arena_reset(void); */
__moo_arena_reset:
	la	$t0, __moo_arena_ptr
	la	$t1, __moo_arena
	sw	$t1, 0($t0)
	jr	$ra

/****************************************************************
 * Source coroutines (sources.md D4)
 *
 * The pair from runtime/start.S, in o32 terms.  A source body runs on its
 * own stack, so a switch saves the callee-saved registers of the side being
 * left, swaps $sp and $fp (kept in the struct), and restores the other
 * side's.
 *
 * struct exc_src (runtime/libexc.h wire format, allocated by __exc_src_new in
 * libexc.c): 0 state (0 new, 1 running, 2 done), 4 body fn, 8 body sp,
 * 12 body fp, 16 pump sp, 20 pump fp, 24 stack top, 28.. argument words.
 *
 * Save area: $ra and $s0-$s7 (9 words at 0..35, one pad word), then the six
 * callee-saved doubles $f20..$f30 8-aligned at 40..87.  Padded to 96, a
 * multiple of 16.
 ****************************************************************/

.macro save_ctx
	addiu	$sp, $sp, -96
	sw	$ra, 0($sp)
	sw	$s0, 4($sp)
	sw	$s1, 8($sp)
	sw	$s2, 12($sp)
	sw	$s3, 16($sp)
	sw	$s4, 20($sp)
	sw	$s5, 24($sp)
	sw	$s6, 28($sp)
	sw	$s7, 32($sp)
	s.d	$f20, 40($sp)
	s.d	$f22, 48($sp)
	s.d	$f24, 56($sp)
	s.d	$f26, 64($sp)
	s.d	$f28, 72($sp)
	s.d	$f30, 80($sp)
.endm

.macro restore_ctx
	lw	$ra, 0($sp)
	lw	$s0, 4($sp)
	lw	$s1, 8($sp)
	lw	$s2, 12($sp)
	lw	$s3, 16($sp)
	lw	$s4, 20($sp)
	lw	$s5, 24($sp)
	lw	$s6, 28($sp)
	lw	$s7, 32($sp)
	l.d	$f20, 40($sp)
	l.d	$f22, 48($sp)
	l.d	$f24, 56($sp)
	l.d	$f26, 64($sp)
	l.d	$f28, 72($sp)
	l.d	$f30, 80($sp)
	addiu	$sp, $sp, 96
.endm

/* int __exc_src_next(struct exc_src *s);
 *   Launches or resumes the body on its private stack and returns the
 *   yielded value (the null-word maybe protocol), or 0 when exhausted. */
__exc_src_next:
	lw	$t0, 0($a0)
	li	$t1, 2
	beq	$t0, $t1, .Lsrc_done	/* exhausted stays exhausted */

	save_ctx
	sw	$sp, 16($a0)		/* pump sp */
	sw	$fp, 20($a0)		/* pump fp */

	lw	$t0, 0($a0)
	bnez	$t0, .Lsrc_resume

	/* first pump: start the body on the top of its own stack */
	li	$t0, 1
	sw	$t0, 0($a0)
	lw	$sp, 24($a0)		/* stack top */
	li	$t1, -16
	and	$sp, $sp, $t1
	addiu	$sp, $sp, -16		/* argument home for the body */
	move	$fp, $sp
	move	$s0, $a0		/* preserve the source across the body */
	lw	$t0, 4($a0)		/* body function; a0 = source (arg0) */
	jalr	$t0

	/* the body returned: exhausted */
	move	$a0, $s0
	li	$t0, 2
	sw	$t0, 0($a0)
	lw	$sp, 16($a0)
	lw	$fp, 20($a0)
	restore_ctx
	li	$v0, 0
	jr	$ra

.Lsrc_resume:
	lw	$sp, 8($a0)		/* body sp */
	lw	$fp, 12($a0)		/* body fp */
	restore_ctx
	/* returns into __exc_src_yield's switch point, whose $v0 is the
	   value the pump hands back */
	jr	$ra

.Lsrc_done:
	li	$v0, 0
	jr	$ra

/* void __exc_src_yield(struct exc_src *s, int value);
 *   Produces a value: park the body and return to the pump. */
__exc_src_yield:
	save_ctx
	sw	$sp, 8($a0)		/* body sp */
	sw	$fp, 12($a0)		/* body fp */
	lw	$sp, 16($a0)		/* pump sp */
	lw	$fp, 20($a0)		/* pump fp */
	move	$v0, $a1		/* the pump's result */
	restore_ctx
	jr	$ra

	.data
	.align	2
__moo_arena_ptr:
	.word	__moo_arena

	.bss
	.align	2
__moo_arena:
	.space	1048576
	.space	262144
stack_top:
