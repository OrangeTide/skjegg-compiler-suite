/* start_rv_psabi_cc.S - RISC-V RV32 C runtime, standard ILP32 psABI.
 *
 * The C runtime for skj-cc-rv, the psABI build of the RISC-V C
 * compiler.  That build emits the standard RISC-V ILP32 register calling
 * convention (a0..a7), so its output interlinks with gcc-built objects.  The
 * only thing that changes from the stack-convention runtime (start_rv.S) is
 * the C-level ABI: the syscall wrappers take their arguments in registers, so
 * they issue ecall directly instead of reloading from the stack.
 *
 *   a7 = syscall number
 *   a0..a5 = args
 *   ecall to invoke
 *   return value in a0
 *
 * The 64-bit integer helpers (__muldi3, the shift family) are an internal ABI
 * between the backend and its runtime, not the C ABI: the backend passes their
 * arguments on the stack regardless of CC_PSABI, so they are byte-identical to
 * start_rv.S and are reproduced here (a C crt does not need the MooScript arena
 * or continuation helpers, which are omitted).
 *
 * _start passes argc=argv=0 (the tests take no arguments), calls main, then
 * passes main's return value to exit.
 */

	.text
	.globl _start
	.globl write
	.globl read
	.globl exit
	.globl _exit

_start:
	la	sp, stack_top
	addi	sp, sp, -16
	li	a0, 0			/* argc */
	li	a1, 0			/* argv */
	jal	ra, main
	li	a7, 93
	ecall
1:	j	1b

/* int write(int fd, const char *buf, int n); */
write:
	li	a7, 64
	ecall
	ret

/* int read(int fd, char *buf, int n); */
read:
	li	a7, 63
	ecall
	ret

/* void exit(int status); */
exit:
_exit:
	li	a7, 93
	ecall
1:	j	1b

/* __muldi3(a_lo, a_hi, b_lo, b_hi) -> a0:a1
 *   64-bit multiply (low 64 bits of result).
 *   Stack: 8(sp)=a_lo, 12(sp)=a_hi, 0(sp)=b_lo, 4(sp)=b_hi
 *   Result: a0=lo, a1=hi */
	.globl __muldi3
__muldi3:
	lw	a0, 8(sp)		/* a0 = a_lo */
	lw	a1, 12(sp)		/* a1 = a_hi */
	lw	a2, 0(sp)		/* a2 = b_lo */
	lw	a3, 4(sp)		/* a3 = b_hi */
	mulhu	t0, a0, a2		/* t0 = hi32(a_lo * b_lo) */
	mul	t1, a1, a2		/* t1 = lo32(a_hi * b_lo) */
	add	t0, t0, t1
	mul	t1, a0, a3		/* t1 = lo32(a_lo * b_hi) */
	add	a1, t0, t1		/* a1 = result_hi */
	mul	a0, a0, a2		/* a0 = result_lo */
	ret

/* __ashldi3(a_lo, a_hi, shift) -> a0:a1
 *   64-bit left shift.
 *   Stack: 0(sp)=shift, 4(sp)=a_lo, 8(sp)=a_hi */
	.globl __ashldi3
__ashldi3:
	lw	a2, 0(sp)		/* a2 = shift */
	lw	a0, 4(sp)		/* a0 = a_lo */
	lw	a1, 8(sp)		/* a1 = a_hi */
	li	t0, 32
	bge	a2, t0, .Lashl_ge32
	beqz	a2, .Lashl_done
	/* shift < 32 */
	sll	a1, a1, a2		/* a1 = a_hi << n */
	sub	t1, t0, a2		/* t1 = 32 - n */
	srl	t2, a0, t1		/* t2 = a_lo >> (32-n) */
	or	a1, a1, t2		/* a1 |= carry bits */
	sll	a0, a0, a2		/* a0 = a_lo << n */
	ret
.Lashl_ge32:
	li	t0, 64
	bge	a2, t0, .Lashl_zero
	addi	a2, a2, -32
	sll	a1, a0, a2		/* a1 = a_lo << (n-32) */
	li	a0, 0
	ret
.Lashl_zero:
	li	a0, 0
	li	a1, 0
.Lashl_done:
	ret

/* __ashrdi3(a_lo, a_hi, shift) -> a0:a1
 *   64-bit arithmetic right shift. */
	.globl __ashrdi3
__ashrdi3:
	lw	a2, 0(sp)		/* a2 = shift */
	lw	a0, 4(sp)		/* a0 = a_lo */
	lw	a1, 8(sp)		/* a1 = a_hi */
	li	t0, 32
	bge	a2, t0, .Lashr_ge32
	beqz	a2, .Lashr_done
	/* shift < 32 */
	srl	a0, a0, a2		/* a0 = a_lo >> n */
	sub	t1, t0, a2		/* t1 = 32 - n */
	sll	t2, a1, t1		/* t2 = a_hi << (32-n) */
	or	a0, a0, t2		/* a0 |= carry bits */
	sra	a1, a1, a2		/* a1 = a_hi >> n (arithmetic) */
	ret
.Lashr_ge32:
	li	t0, 64
	bge	a2, t0, .Lashr_sign
	addi	a2, a2, -32
	sra	a0, a1, a2		/* a0 = a_hi >> (n-32) */
	srai	a1, a1, 31		/* sign extension */
	ret
.Lashr_sign:
	srai	a1, a1, 31
	mv	a0, a1
.Lashr_done:
	ret

/* __lshrdi3(a_lo, a_hi, shift) -> a0:a1
 *   64-bit logical right shift. */
	.globl __lshrdi3
__lshrdi3:
	lw	a2, 0(sp)		/* a2 = shift */
	lw	a0, 4(sp)		/* a0 = a_lo */
	lw	a1, 8(sp)		/* a1 = a_hi */
	li	t0, 32
	bge	a2, t0, .Llshr_ge32
	beqz	a2, .Llshr_done
	/* shift < 32 */
	srl	a0, a0, a2		/* a0 = a_lo >> n */
	sub	t1, t0, a2		/* t1 = 32 - n */
	sll	t2, a1, t1		/* t2 = a_hi << (32-n) */
	or	a0, a0, t2
	srl	a1, a1, a2		/* a1 = a_hi >> n */
	ret
.Llshr_ge32:
	li	t0, 64
	bge	a2, t0, .Llshr_zero
	addi	a2, a2, -32
	srl	a0, a1, a2		/* a0 = a_hi >> (n-32) */
	li	a1, 0
	ret
.Llshr_zero:
	li	a0, 0
	li	a1, 0
.Llshr_done:
	ret

	.bss
	.align	4
	.space	196608
stack_top:
