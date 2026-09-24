/* start_mips.S - minimal MIPS I (o32, little-endian) runtime for Linux
 * user-mode, as emulated by qemu-mipsel.
 *
 * Provides _start (the ELF entry point) and thin wrappers for the two
 * syscalls the test programs need: write(2) and exit(2).  qemu-mipsel
 * uses the Linux/MIPS o32 syscall convention:
 *
 *   $v0 = syscall number (the o32 space starts at 4000)
 *   $a0..$a3 = args
 *   syscall to invoke
 *   return value in $v0
 *
 * The backend emits the o32 platform psABI, so arguments arrive in
 * $a0..$a3.  The syscall wrappers pass them straight through: write's
 * (fd, buf, n) are already in $a0, $a1, $a2, which is the syscall order.
 *
 * _start drops argc/argv (tests take no arguments), calls main, then
 * passes main's return value to _exit.
 *
 * The file relies on the assembler's default `.set reorder`: for
 * -march=mips1 the assembler fills the load delay slot, the branch delay
 * slot, and the mult/div latency, matching what the code generator emits.
 */

	.set reorder

	.text
	.globl _start
	.globl write
	.globl read
	.globl exit
	.globl _exit

_start:
	la	$sp, stack_top
	addiu	$sp, $sp, -16
	jal	main
	move	$a0, $v0		/* exit status = main's return */
	li	$v0, 4001		/* __NR_exit */
	syscall
1:	b	1b

/* int write(int fd, const char *buf, int n); */
write:
	li	$v0, 4004		/* __NR_write */
	syscall
	jr	$ra

/* int read(int fd, char *buf, int n); */
read:
	li	$v0, 4003		/* __NR_read */
	syscall
	jr	$ra

/* void exit(int status); */
exit:
_exit:
	li	$v0, 4001		/* __NR_exit */
	syscall
1:	b	1b

/* 64-bit integer helpers, called under our stack convention and returning
 * the pair in $v0 (low) : $v1 (high).  Ported from start_rv.S; pure integer,
 * leaf, so they save nothing. */

/* __muldi3(b_lo, b_hi, a_lo, a_hi) -> $v0:$v1   (low 64 bits of a*b)
 *   Stack on entry: 0=b_lo, 4=b_hi, 8=a_lo, 12=a_hi */
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

/* __ashldi3(shift, a_lo, a_hi) -> $v0:$v1   (64-bit left shift)
 *   Stack on entry: 0=shift, 4=a_lo, 8=a_hi */
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

/* void *__moo_arena_alloc(int size);
 *   Bump allocator.  Aligns size up to 4 bytes.  Returns pointer in $v0. */
	.globl __moo_arena_alloc
__moo_arena_alloc:
	addiu	$a0, $a0, 3
	li	$t1, -4
	and	$a0, $a0, $t1		/* round up to 4 (andi zero-extends) */
	la	$t0, __moo_arena_ptr
	lw	$t2, 0($t0)		/* $t2 = old pointer (return value) */
	addu	$t3, $t2, $a0		/* $t3 = new pointer */
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

/* __cont_capture()
 *   Called by IR_CAPTURE inline code after pushing $s0-$s7.
 *   The jal return address ($ra) is on the stack as part of the captured
 *   segment (pushed here).
 *
 *   Reads the 12-byte mark slot via __cont_mark_sp, bump-allocates a buffer
 *   in __cont_arena, copies the stack segment from capture_sp to mark_sp,
 *   then longjmps to the mark's re-entry label with $v0 = buffer address
 *   (nonzero).
 *
 *   On entry:  $s0-$s7 on stack (pushed by IR_CAPTURE inline code)
 *              $ra = return address to IR_CAPTURE resume point
 *   Clobbers:  all regs (callee-saves are in the captured segment) */
	.globl __cont_capture
__cont_capture:
	addiu	$sp, $sp, -4
	sw	$ra, 0($sp)		/* save return address on stack */
	move	$t2, $sp		/* $t2 = capture_sp */
	move	$t3, $fp		/* $t3 = capture_fp */

	/* read 12-byte mark slot */
	la	$t0, __cont_mark_sp
	lw	$t0, 0($t0)		/* $t0 = mark slot pointer */
	lw	$t4, 0($t0)		/* $t4 = mark's saved_fp */
	lw	$t5, 4($t0)		/* $t5 = mark's saved_sp */
	lw	$t6, 8($t0)		/* $t6 = mark's re-entry PC */

	/* size = mark_sp - capture_sp (stack grows down) */
	subu	$a2, $t5, $t2		/* $a2 = size */

	/* bump-allocate: 12-byte header + size bytes */
	la	$t0, __cont_arena_ptr
	lw	$v0, 0($t0)		/* $v0 = buf */

	/* write buffer header [capture_sp, capture_fp, size] */
	sw	$t2, 0($v0)
	sw	$t3, 4($v0)
	sw	$a2, 8($v0)

	/* advance arena pointer past this buffer */
	addu	$t1, $v0, $a2
	addiu	$t1, $t1, 12
	sw	$t1, 0($t0)

	/* copy stack segment: capture_sp -> buf+12 */
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

	/* longjmp to mark: restore FP/SP, jump to re-entry label */
	move	$fp, $t4		/* restore mark's FP */
	move	$sp, $t5		/* restore mark's SP */
	jr	$t6			/* $v0 = buf (nonzero) */

/* __cont_resume(buf, value)
 *   Restores the captured stack segment and returns into it.  The saved $ra
 *   on the stack (pushed by __cont_capture) is popped by the epilogue,
 *   landing at the IR_CAPTURE resume point.
 *
 *   On entry:  0($sp) = buf pointer, 4($sp) = resume value */
	.globl __cont_resume
__cont_resume:
	lw	$a3, 0($sp)		/* $a3 = buf */
	lw	$a1, 4($sp)		/* $a1 = resume value */

	/* read buffer header */
	lw	$t2, 0($a3)		/* $t2 = capture_sp */
	lw	$t3, 4($a3)		/* $t3 = capture_fp */
	lw	$a2, 8($a3)		/* $a2 = size */

	/* restore FP and SP to captured state */
	move	$fp, $t3
	move	$sp, $t2

	/* copy stack data back: buf+12 -> capture_sp */
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
	.globl	__cont_arena_ptr
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
stack_top:
