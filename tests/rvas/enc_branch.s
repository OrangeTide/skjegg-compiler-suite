# Encoding golden-master: branches, branch pseudos and jumps to labels.
	.text
f:
	beq	a0, a1, .L1
	bne	a0, a1, .L2
	blt	a0, a1, .L1
	bge	a0, a1, .L2
	bltu	a0, a1, .L1
	bgeu	a0, a1, .L2
	bgt	a0, a1, .L1
	ble	a0, a1, .L2
	beqz	a0, .L1
	bnez	a0, .L2
	bgez	a0, .L1
	blez	a0, .L2
	bgtz	a0, .L1
	bltz	a0, .L2
.L1:
	j	.L2
	jal	ra, f
.L2:
	jal	f
	nop