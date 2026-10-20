/* setjmp_mips.S : MIPS I (o32, little-endian) setjmp/longjmp.  jmp_buf (a
 * long[32]) holds the callee-saved state longjmp must restore: $s0-$s7, the
 * frame pointer $fp, the stack pointer $sp, and the return address $ra.  Float
 * ($f20-$f30) is not saved: the compiler the self-host build runs does not keep
 * live fp registers across a setjmp, the same assumption the RV32 twin
 * (setjmp_rv.S) makes.  setjmp returns 0 on the direct call; longjmp resumes it
 * returning val (or 1 if val is 0).  o32 passes env in $a0 and val in $a1.
 */

	.set reorder

	.text
	.globl setjmp
	.globl longjmp

	.ent setjmp
setjmp:
	sw	$s0, 0($a0)
	sw	$s1, 4($a0)
	sw	$s2, 8($a0)
	sw	$s3, 12($a0)
	sw	$s4, 16($a0)
	sw	$s5, 20($a0)
	sw	$s6, 24($a0)
	sw	$s7, 28($a0)
	sw	$fp, 32($a0)
	sw	$sp, 36($a0)
	sw	$ra, 40($a0)
	move	$v0, $zero		/* direct call returns 0 */
	jr	$ra
	.end setjmp

	.ent longjmp
longjmp:
	lw	$s0, 0($a0)
	lw	$s1, 4($a0)
	lw	$s2, 8($a0)
	lw	$s3, 12($a0)
	lw	$s4, 16($a0)
	lw	$s5, 20($a0)
	lw	$s6, 24($a0)
	lw	$s7, 28($a0)
	lw	$fp, 32($a0)
	lw	$sp, 36($a0)
	lw	$ra, 40($a0)
	move	$v0, $a1		/* return val ... */
	bnez	$v0, .Llongjmp_ret
	li	$v0, 1			/* ... but never 0 */
.Llongjmp_ret:
	jr	$ra
	.end longjmp
