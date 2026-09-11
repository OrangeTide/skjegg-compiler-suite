/* start_psx.S - MIPS I (o32, little-endian) runtime for the PlayStation
 * (R3051), loaded as a PS-EXE.
 *
 * This is the BIOS-TTY counterpart of start_mips.S.  The difference is the
 * three entry points that touch the outside world: _start, write, and exit.
 * On Linux (start_mips.S) they are the write(2)/exit(2) syscalls qemu-mipsel
 * services; here they go through the PlayStation BIOS call tables, so the same
 * compiled programs run on real R3051 hardware (or an emulator that provides
 * the BIOS).  Everything below the entry points, the 64-bit integer helpers,
 * the bump arena, and the continuation capture/resume, is pure computation and
 * is identical to start_mips.S.
 *
 * BIOS calls: a function is dispatched by jumping to 0xA0/0xB0/0xC0 with its
 * number in $t1.  This runtime uses A0(3Ch) std_out_putchar for TTY output.
 * The PS-EXE loader enters _start with $sp already set from the header's stack
 * base (0x801ffff0); _start sets it again so the crt does not depend on the
 * loader honoring that field.  bss is cleared here (the header also names it so
 * a loader may pre-clear it; doing it too is harmless and keeps the crt
 * self-sufficient).  There is no process to exit to on the PlayStation, so
 * exit halts in an idle loop.
 *
 * Relies on the assembler's default `.set reorder' for delay-slot filling, as
 * start_mips.S does.
 */

	.set reorder

	.text
	.globl _start
	.globl write
	.globl exit
	.globl _exit

_start:
	li	$sp, 0x801ffff0		/* top of 2 MB RAM, less a guard */
	li	$gp, 0

	/* zero the bss the linker script delimits */
	la	$t0, __bss_start
	la	$t1, __bss_end
.Lbss_loop:
	bgeu	$t0, $t1, .Lbss_done
	sw	$zero, 0($t0)
	addiu	$t0, $t0, 4
	b	.Lbss_loop
.Lbss_done:

	addiu	$sp, $sp, -16
	jal	main
	/* pcsx_exit(main's return): a halfword store to 0x1f802082 tells
	   pcsx-redux to quit with this code, which is how a PS-EXE self-checks
	   under the emulator.  On real hardware the address is an unused I/O
	   slot, so the store is harmless and execution falls into the halt. */
	li	$t0, 0x1f802082
	sh	$v0, 0($t0)
1:	b	1b

/* int write(int fd, const char *buf, int n);
 * fd is ignored; each byte goes to the BIOS TTY via A0(3Ch).  Args arrive on
 * the stack (our calling convention); $s0-$s2 hold the loop state across the
 * BIOS call, which preserves the callee-saved registers. */
write:
	addiu	$sp, $sp, -20
	sw	$ra, 0($sp)
	sw	$s0, 4($sp)
	sw	$s1, 8($sp)
	sw	$s2, 12($sp)
	lw	$s0, 24($sp)		/* buf (caller's 4($sp)) */
	lw	$s1, 28($sp)		/* n   (caller's 8($sp)) */
	move	$s2, $zero		/* i = 0 */
.Lwrite_loop:
	bgeu	$s2, $s1, .Lwrite_done
	addu	$t0, $s0, $s2
	lbu	$a0, 0($t0)		/* buf[i] */
	li	$t1, 0x3c		/* A0(3Ch) std_out_putchar */
	li	$t2, 0xa0
	jalr	$t2
	addiu	$s2, $s2, 1
	b	.Lwrite_loop
.Lwrite_done:
	move	$v0, $s1		/* return the byte count */
	lw	$ra, 0($sp)
	lw	$s0, 4($sp)
	lw	$s1, 8($sp)
	lw	$s2, 12($sp)
	addiu	$sp, $sp, 20
	jr	$ra

/* void exit(int status); the PlayStation has no process to return to, so this
 * signals pcsx_exit (see _start) and then halts. */
exit:
_exit:
	lw	$a0, 0($sp)		/* status (our stack convention) */
	li	$t0, 0x1f802082
	sh	$a0, 0($t0)		/* pcsx_exit(status) */
1:	b	1b

/* 64-bit integer helpers, called under our stack convention and returning
 * the pair in $v0 (low) : $v1 (high).  Identical to start_mips.S. */

/* __muldi3(b_lo, b_hi, a_lo, a_hi) -> $v0:$v1   (low 64 bits of a*b) */
	.globl __muldi3
__muldi3:
	lw	$t0, 8($sp)		/* a_lo */
	lw	$t1, 12($sp)		/* a_hi */
	lw	$t2, 0($sp)		/* b_lo */
	lw	$t3, 4($sp)		/* b_hi */
	multu	$t0, $t2		/* a_lo * b_lo */
	mflo	$v0			/* result_lo = lo32(a_lo*b_lo) */
	mfhi	$t4			/* hi32(a_lo*b_lo) */
	mul	$t5, $t1, $t2		/* lo32(a_hi*b_lo) */
	addu	$t4, $t4, $t5
	mul	$t5, $t0, $t3		/* lo32(a_lo*b_hi) */
	addu	$v1, $t4, $t5		/* result_hi */
	jr	$ra

/* __ashldi3(shift, a_lo, a_hi) -> $v0:$v1   (64-bit left shift) */
	.globl __ashldi3
__ashldi3:
	lw	$a2, 0($sp)		/* shift */
	lw	$v0, 4($sp)		/* a_lo */
	lw	$v1, 8($sp)		/* a_hi */
	li	$t0, 32
	bge	$a2, $t0, .Lashl_ge32
	beqz	$a2, .Lashl_done
	sllv	$v1, $v1, $a2		/* hi << n */
	subu	$t1, $t0, $a2		/* 32 - n */
	srlv	$t2, $v0, $t1		/* lo >> (32-n) */
	or	$v1, $v1, $t2
	sllv	$v0, $v0, $a2		/* lo << n */
	jr	$ra
.Lashl_ge32:
	li	$t0, 64
	bge	$a2, $t0, .Lashl_zero
	addiu	$a2, $a2, -32
	sllv	$v1, $v0, $a2		/* hi = lo << (n-32) */
	li	$v0, 0
	jr	$ra
.Lashl_zero:
	li	$v0, 0
	li	$v1, 0
.Lashl_done:
	jr	$ra

/* __ashrdi3(shift, a_lo, a_hi) -> $v0:$v1   (64-bit arithmetic right shift) */
	.globl __ashrdi3
__ashrdi3:
	lw	$a2, 0($sp)
	lw	$v0, 4($sp)
	lw	$v1, 8($sp)
	li	$t0, 32
	bge	$a2, $t0, .Lashr_ge32
	beqz	$a2, .Lashr_done
	srlv	$v0, $v0, $a2		/* lo >> n */
	subu	$t1, $t0, $a2		/* 32 - n */
	sllv	$t2, $v1, $t1		/* hi << (32-n) */
	or	$v0, $v0, $t2
	srav	$v1, $v1, $a2		/* hi >> n (arithmetic) */
	jr	$ra
.Lashr_ge32:
	li	$t0, 64
	bge	$a2, $t0, .Lashr_sign
	addiu	$a2, $a2, -32
	srav	$v0, $v1, $a2		/* lo = hi >> (n-32) */
	sra	$v1, $v1, 31		/* sign extension */
	jr	$ra
.Lashr_sign:
	sra	$v1, $v1, 31
	move	$v0, $v1
.Lashr_done:
	jr	$ra

/* __lshrdi3(shift, a_lo, a_hi) -> $v0:$v1   (64-bit logical right shift) */
	.globl __lshrdi3
__lshrdi3:
	lw	$a2, 0($sp)
	lw	$v0, 4($sp)
	lw	$v1, 8($sp)
	li	$t0, 32
	bge	$a2, $t0, .Llshr_ge32
	beqz	$a2, .Llshr_done
	srlv	$v0, $v0, $a2		/* lo >> n */
	subu	$t1, $t0, $a2		/* 32 - n */
	sllv	$t2, $v1, $t1		/* hi << (32-n) */
	or	$v0, $v0, $t2
	srlv	$v1, $v1, $a2		/* hi >> n */
	jr	$ra
.Llshr_ge32:
	li	$t0, 64
	bge	$a2, $t0, .Llshr_zero
	addiu	$a2, $a2, -32
	srlv	$v0, $v1, $a2		/* lo = hi >> (n-32) */
	li	$v1, 0
	jr	$ra
.Llshr_zero:
	li	$v0, 0
	li	$v1, 0
.Llshr_done:
	jr	$ra

/* void *__moo_arena_alloc(int size);  bump allocator, 4-byte aligned. */
	.globl __moo_arena_alloc
__moo_arena_alloc:
	lw	$a0, 0($sp)
	addiu	$a0, $a0, 3
	li	$t1, -4
	and	$a0, $a0, $t1		/* round up to 4 */
	la	$t0, __moo_arena_ptr
	lw	$t2, 0($t0)		/* old pointer (return value) */
	addu	$t3, $t2, $a0		/* new pointer */
	sw	$t3, 0($t0)
	move	$v0, $t2
	jr	$ra

/* void __moo_arena_reset(void); */
	.globl __moo_arena_reset
__moo_arena_reset:
	la	$t0, __moo_arena_ptr
	la	$t1, __moo_arena
	sw	$t1, 0($t0)
	jr	$ra

/* __cont_capture(): see start_mips.S for the full contract. */
	.globl __cont_capture
__cont_capture:
	addiu	$sp, $sp, -4
	sw	$ra, 0($sp)		/* save return address on stack */
	move	$t2, $sp		/* $t2 = capture_sp */
	move	$t3, $fp		/* $t3 = capture_fp */

	la	$t0, __cont_mark_sp
	lw	$t0, 0($t0)		/* $t0 = mark slot pointer */
	lw	$t4, 0($t0)		/* $t4 = mark's saved_fp */
	lw	$t5, 4($t0)		/* $t5 = mark's saved_sp */
	lw	$t6, 8($t0)		/* $t6 = mark's re-entry PC */

	subu	$a2, $t5, $t2		/* $a2 = size */

	la	$t0, __cont_arena_ptr
	lw	$v0, 0($t0)		/* $v0 = buf */

	sw	$t2, 0($v0)
	sw	$t3, 4($v0)
	sw	$a2, 8($v0)

	addu	$t1, $v0, $a2
	addiu	$t1, $t1, 12
	sw	$t1, 0($t0)

	addiu	$a3, $v0, 12		/* $a3 = dst (buf + 12) */
	move	$a1, $t2		/* $a1 = src (capture_sp) */
	move	$a0, $a2		/* $a0 = remaining bytes */
	beqz	$a0, .Lcap_done
.Lcap_loop:
	lw	$t1, 0($a1)
	sw	$t1, 0($a3)
	addiu	$a1, $a1, 4
	addiu	$a3, $a3, 4
	addiu	$a0, $a0, -4
	bnez	$a0, .Lcap_loop
.Lcap_done:

	move	$fp, $t4		/* restore mark's FP */
	move	$sp, $t5		/* restore mark's SP */
	jr	$t6			/* $v0 = buf (nonzero) */

/* __cont_resume(buf, value): see start_mips.S for the full contract. */
	.globl __cont_resume
__cont_resume:
	lw	$a3, 0($sp)		/* $a3 = buf */
	lw	$a1, 4($sp)		/* $a1 = resume value */

	lw	$t2, 0($a3)		/* $t2 = capture_sp */
	lw	$t3, 4($a3)		/* $t3 = capture_fp */
	lw	$a2, 8($a3)		/* $a2 = size */

	move	$fp, $t3
	move	$sp, $t2

	addiu	$t4, $a3, 12		/* $t4 = src (buf + 12) */
	move	$t5, $sp		/* $t5 = dst (capture_sp) */
	beqz	$a2, .Lres_done
.Lres_loop:
	lw	$t1, 0($t4)
	sw	$t1, 0($t5)
	addiu	$t4, $t4, 4
	addiu	$t5, $t5, 4
	addiu	$a2, $a2, -4
	bnez	$a2, .Lres_loop
.Lres_done:

	move	$v0, $a1		/* $v0 = resume value */
	lw	$ra, 0($sp)		/* restore $ra (saved by __cont_capture) */
	addiu	$sp, $sp, 4
	jr	$ra			/* return to IR_CAPTURE resume point */

	.data
	.align	2
	.globl __cont_mark_sp
__cont_mark_sp:
	.word	0
__cont_arena_ptr:
	.word	__cont_arena
__moo_arena_ptr:
	.word	__moo_arena

	.bss
	.align	2
__cont_arena:
	.space	65536
__moo_arena:
	.space	65536
	.space	65536
