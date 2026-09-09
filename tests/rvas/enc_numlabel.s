/* Multi-digit numeric local labels (GAS 1: / 42: / 1000:, referenced f/b).
 * Byte-compared against GNU as, so a mismatch is a label-resolution bug:
 * a forward reference binds to the next definition, a backward one to the
 * most recent, and a number may be redefined any number of times. */
	.text
	.globl _start
_start:
	li	a0, 0
	li	t0, 5
42:
	addi	a0, a0, 1
	addi	t0, t0, -1
	bnez	t0, 42b			/* backward to the nearest 42: */
	j	1000f			/* forward, multi-digit */
	li	a0, 99
1000:
	addi	a0, a0, 37
	j	7f
	li	a0, 88
7:
	beqz	a0, 42f			/* forward to the redefined 42: below */
	nop
42:					/* 42 redefined; f above binds here */
	li	a7, 93
	ecall

/* the same number reused far away, both directions resolving locally */
loop2:
	li	t1, 3
123:
	addi	t1, t1, -1
	bnez	t1, 123b
	li	t2, 0
	beqz	t2, 123f
	nop
123:
	ret
