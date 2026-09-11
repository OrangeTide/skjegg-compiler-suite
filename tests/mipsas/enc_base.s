# Encoding fixture: the base integer and COP1 instruction set.
# Assembled by skj-as-mips and by GNU as, the section bytes compared.
# .set noreorder so no delay-slot scheduling perturbs the comparison.
	.set noreorder
	.text
	.globl f
f:
	addu $t0, $t1, $t2
	subu $s0, $s1, $s2
	and $v0, $v1, $a0
	or $a1, $a2, $a3
	xor $t3, $t4, $t5
	nor $t6, $t7, $t8
	slt $s3, $s4, $s5
	sltu $s6, $s7, $t9
	sllv $t0, $t1, $t2
	srlv $t3, $t4, $t5
	srav $t6, $t7, $t8
	sll $t0, $t1, 5
	srl $t2, $t3, 7
	sra $t4, $t5, 31
	addiu $sp, $sp, -48
	addiu $t0, $t1, 32000
	sltiu $v0, $v1, 100
	slti $a0, $a1, -5
	andi $t0, $t1, 255
	ori $t2, $t3, 65535
	xori $t4, $t5, 4096
	lui $t6, 4660
	mult $t0, $t1
	multu $t2, $t3
	mfhi $s0
	mflo $s1
	mthi $s2
	mtlo $s3
	lw $t0, 44($sp)
	sw $ra, 40($sp)
	lhu $a0, 2($t1)
	lh $a1, 4($t2)
	lbu $a2, 1($t3)
	lb $a3, 0($t4)
	sh $t5, 6($t6)
	sb $t7, 3($t8)
	nop
	jr $ra
	jalr $t9
	jalr $ra, $t9
# COP1
	mtc1 $t0, $f2
	mfc1 $t1, $f4
	cfc1 $t3, $31
	ctc1 $t5, $31
	lwc1 $f6, 8($sp)
	swc1 $f8, 12($sp)
	add.d $f0, $f2, $f4
	sub.d $f6, $f8, $f10
	mul.s $f12, $f14, $f16
	div.d $f18, $f20, $f22
	mov.d $f24, $f26
	neg.d $f28, $f30
	abs.d $f0, $f2
	cvt.s.d $f0, $f2
	cvt.d.s $f4, $f6
	cvt.w.d $f8, $f10
	cvt.d.w $f12, $f14
	c.eq.d $f20, $f22
	c.lt.d $f24, $f26
	c.le.d $f28, $f30
