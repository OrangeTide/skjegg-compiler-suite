# Exercises conditional-branch relaxation end to end: the taken branch target
# sits past the B-type +/-4 KB range, so the assembler must relax it to an
# inverted branch over a jal.  Control must still reach .target (exit 0); the
# zero padding is jumped over and never executed.
	.text
	.globl _start
_start:
	li	t0, 1
	bnez	t0, .target       # taken; target is far, forcing relaxation
	li	a0, 99            # fall-through means relaxation broke control flow
	j	.end
.gap:
	.space	6000
.target:
	li	a0, 0
.end:
	li	a7, 93
	ecall
