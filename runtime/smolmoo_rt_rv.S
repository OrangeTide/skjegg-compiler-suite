/* smolmoo_rt_rv.S : RV32 runtime assembly for Excelsior on smolmoo
 * (https://github.com/OrangeTide/smolmoo), the RISC-V counterpart of
 * smolmoo_rt.S.
 *
 * The smolmoo build links no crt (no _start, no Linux syscalls: the
 * binding's C _start in exc_smolmoo.c enters at .text.entry, the host
 * has already seeded sp, and the arena is C), so only the source
 * coroutine helpers live here. They are the __exc_src_next /
 * __exc_src_yield pair from runtime/start_rv_psabi.S (sources.md D4;
 * keep the three copies in sync: start.S ColdFire, start_rv_psabi.S
 * RV32, and here). The save/restore frame and the struct exc_src wire
 * format below are identical to start_rv_psabi.S's.
 *
 * struct exc_src (runtime/libexc.h wire format, allocated by
 * __exc_src_new in libexc.c): 0 state (0 new, 1 running, 2 done),
 * 4 body fn, 8 body sp, 12 body fp, 16 pump sp, 20 pump fp,
 * 24 stack top, 28.. argument words.
 *
 * Save area: ra and s1-s11 at 0..47, fs0-fs11 at 48..143, 144 bytes.
 * The fsd/fld halves need the D extension; a verb that actually pumps a
 * source therefore requires the smolmoo core to have RV32D. A verb that
 * does not use sources never reaches this code (it is linked to satisfy
 * libexc's references, not executed).
 */

	.text

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
	.globl __exc_src_next
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
	.globl __exc_src_yield
__exc_src_yield:
	save_ctx
	sw	sp, 8(a0)		/* body sp */
	sw	s0, 12(a0)		/* body fp */
	lw	sp, 16(a0)		/* pump sp */
	lw	s0, 20(a0)		/* pump fp */
	mv	a0, a1			/* the pump's result */
	restore_ctx
	ret
