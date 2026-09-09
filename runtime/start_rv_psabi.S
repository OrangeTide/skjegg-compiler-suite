/* start_rv_psabi.S - RV32 runtime for the platform-ABI tier.
 *
 * runtime/start_rv.S serves the toolkit's own stack convention: its
 * write() and helpers read their arguments off the stack, which is what
 * a non-psABI build of the RISC-V backend emits.  A tier that links
 * gcc-built C (libexc and its host binding) needs the same services
 * under the RISC-V ILP32 convention instead, arguments in a0 upward,
 * and that is this file.  The two are never linked together.
 *
 * The 64-bit helpers are not here: under the platform ABI they are
 * ordinary C, so runtime/soft64.c supplies them (the cross toolchain
 * ships no rv32 libgcc to take them from).
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

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

/* Linux/RISC-V syscall numbers: read 63, write 64, exit 93 */

_start:
	la	sp, stack_top		/* our own stack, like start_rv.S: the
					   kernel's is one page and the deeper
					   tests overflow it */
	andi	sp, sp, -16
	jal	ra, main

	li	a7, 93
	ecall
1:	j	1b

/* int read(int fd, char *buf, int n); */
read:
	li	a7, 63
	ecall
	ret

/* int write(int fd, const char *buf, int n); */
write:
	li	a7, 64
	ecall
	ret

/* void exit(int status);  (alias _exit) */
exit:
_exit:
	li	a7, 93
	ecall
1:	j	1b

/* void *__moo_arena_alloc(int size); bump allocator, 4-byte aligned */
__moo_arena_alloc:
	addi	a0, a0, 3
	andi	a0, a0, -4
	la	t0, __moo_arena_ptr
	lw	t1, 0(t0)		/* t1 = old pointer, the result */
	add	t2, t1, a0
	sw	t2, 0(t0)
	mv	a0, t1
	ret

/* void __moo_arena_reset(void); */
__moo_arena_reset:
	la	t0, __moo_arena_ptr
	la	t1, __moo_arena
	sw	t1, 0(t0)
	ret

/****************************************************************
 * Source coroutines (sources.md D4)
 *
 * The pair from runtime/start.S, in RV32 terms.  A source body runs on
 * its own stack, so a switch is: save the callee-saved registers of the
 * side being left, swap sp and fp, restore the other side's.  Keeping
 * the saved set to what the ABI calls callee-saved is what makes the
 * two halves of a switch agree across a call boundary.
 *
 * struct exc_src (runtime/libexc.h wire format, allocated by
 * __exc_src_new in libexc.c): 0 state (0 new, 1 running, 2 done),
 * 4 body fn, 8 body sp, 12 body fp, 16 pump sp, 20 pump fp,
 * 24 stack top, 28.. argument words.
 *
 * Save area: 12 integer registers (ra, s0-s11) at 0..47 and 12 double
 * registers (fs0-fs11) at 48..143, 144 bytes, a multiple of 16.
 ****************************************************************/


.macro save_ctx
	addi	sp, sp, -144
	sw	ra, 0(sp)
	sw	s1, 4(sp)
	sw	s2, 8(sp)
	sw	s3, 12(sp)
	sw	s4, 16(sp)
	sw	s5, 20(sp)
	sw	s6, 24(sp)
	sw	s7, 28(sp)
	sw	s8, 32(sp)
	sw	s9, 36(sp)
	sw	s10, 40(sp)
	sw	s11, 44(sp)
	fsd	fs0, 48(sp)
	fsd	fs1, 56(sp)
	fsd	fs2, 64(sp)
	fsd	fs3, 72(sp)
	fsd	fs4, 80(sp)
	fsd	fs5, 88(sp)
	fsd	fs6, 96(sp)
	fsd	fs7, 104(sp)
	fsd	fs8, 112(sp)
	fsd	fs9, 120(sp)
	fsd	fs10, 128(sp)
	fsd	fs11, 136(sp)
.endm

.macro restore_ctx
	lw	ra, 0(sp)
	lw	s1, 4(sp)
	lw	s2, 8(sp)
	lw	s3, 12(sp)
	lw	s4, 16(sp)
	lw	s5, 20(sp)
	lw	s6, 24(sp)
	lw	s7, 28(sp)
	lw	s8, 32(sp)
	lw	s9, 36(sp)
	lw	s10, 40(sp)
	lw	s11, 44(sp)
	fld	fs0, 48(sp)
	fld	fs1, 56(sp)
	fld	fs2, 64(sp)
	fld	fs3, 72(sp)
	fld	fs4, 80(sp)
	fld	fs5, 88(sp)
	fld	fs6, 96(sp)
	fld	fs7, 104(sp)
	fld	fs8, 112(sp)
	fld	fs9, 120(sp)
	fld	fs10, 128(sp)
	fld	fs11, 136(sp)
	addi	sp, sp, 144
.endm

/* int __exc_src_next(struct exc_src *s);
 *   Launches or resumes the body on its private stack and returns the
 *   yielded value (the null-word maybe protocol), or 0 when exhausted. */
__exc_src_next:
	lw	t0, 0(a0)
	li	t1, 2
	beq	t0, t1, .Lsrc_done	/* exhausted stays exhausted */

	save_ctx
	sw	sp, 16(a0)		/* pump sp */
	sw	s0, 20(a0)		/* pump fp */

	lw	t0, 0(a0)
	bnez	t0, .Lsrc_resume

	/* first pump: start the body on the top of its own stack */
	li	t0, 1
	sw	t0, 0(a0)
	lw	sp, 24(a0)		/* stack top */
	andi	sp, sp, -16
	mv	s0, sp
	lw	t0, 4(a0)		/* body function */
	mv	s1, a0			/* the body reads its source in a0 */
	jalr	ra, t0, 0

	/* the body returned: exhausted */
	mv	a0, s1
	li	t0, 2
	sw	t0, 0(a0)
	lw	sp, 16(a0)
	lw	s0, 20(a0)
	restore_ctx
	li	a0, 0
	ret

.Lsrc_resume:
	lw	sp, 8(a0)		/* body sp */
	lw	s0, 12(a0)		/* body fp */
	restore_ctx
	/* returns into __exc_src_yield's switch point, whose a0 is the
	   value the pump hands back */
	ret

.Lsrc_done:
	li	a0, 0
	ret

/* void __exc_src_yield(struct exc_src *s, int value);
 *   Produces a value: park the body and return to the pump. */
__exc_src_yield:
	save_ctx
	sw	sp, 8(a0)		/* body sp */
	sw	s0, 12(a0)		/* body fp */
	lw	sp, 16(a0)		/* pump sp */
	lw	s0, 20(a0)		/* pump fp */
	mv	a0, a1			/* the pump's result */
	restore_ctx
	ret

	.data
	.align	4
__moo_arena_ptr:
	.word	__moo_arena

	.bss
	.align	4
__moo_arena:
	.space	1048576
	.space	262144
stack_top:
