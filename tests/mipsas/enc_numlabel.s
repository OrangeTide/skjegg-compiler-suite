# Encoding fixture: GAS numeric local labels (N: definitions, Nf/Nb references).
# Assembled by skj-as-mips and by GNU as, the section bytes compared.
# .set noreorder so no delay-slot scheduling perturbs the comparison.
	.set noreorder
	.text
	.globl f
f:
	li $t0, 0
	li $t1, 3
1:				# first definition of label 1
	addiu $t0, $t0, 1
	bne $t0, $t1, 1b	# backward: the nearest 1: above
	nop
	b 1f			# forward: the next 1: below
	nop
1:				# second definition of label 1
	beq $zero, $zero, 2f	# forward across an unrelated number
	nop
	b 1b			# backward: the second 1:
	nop
2:
	move $v0, $t0
	jr $ra
	nop
# a multi-digit number, and a self-loop referencing itself backward
42:
	b 42b
	nop
