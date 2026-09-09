# Encoding golden-master: RV32I base, M, pseudos, loads/stores, CSR, AMO,
# Zba/Zbb/Zbs.  The runner assembles this with skj-as-rv and with the GNU
# assembler and compares the .text bytes.
	.text
	addi	sp, sp, -16
	addi	a0, x0, 5
	lui	t0, 0x10
	auipc	t1, 0
	add	a2, a0, a1
	sub	a3, a0, a1
	sll	a4, a0, a1
	slt	a5, a0, a1
	sltu	a6, a0, a1
	xor	a7, a0, a1
	srl	s2, a0, a1
	sra	s3, a0, a1
	or	s4, a0, a1
	and	s5, a0, a1
	mul	t2, a0, a1
	mulh	t3, a0, a1
	mulhsu	t4, a0, a1
	mulhu	t5, a0, a1
	div	t6, a0, a1
	divu	s6, a0, a1
	rem	s7, a0, a1
	remu	s8, a0, a1
	slli	a4, a0, 3
	srli	a4, a0, 3
	srai	a5, a1, 31
	andi	t2, t2, -4
	ori	t0, t0, 15
	xori	t0, t0, 1
	slti	a0, a1, 7
	sltiu	a0, a1, 7
	lw	s0, 8(sp)
	lb	a0, 0(a1)
	lbu	a0, 1(a1)
	lh	a0, 2(a1)
	lhu	a0, 4(a1)
	sw	s1, 12(sp)
	sb	a0, 0(a1)
	sh	a0, 2(a1)
	jalr	ra, t0, 0
	mv	a0, a1
	neg	a1, a2
	not	a2, a3
	seqz	a3, a4
	snez	a4, a5
	nop
	ret
	jr	t1
	ecall
	ebreak
	li	a0, 0
	li	a1, 0x12345
	li	a2, -1
	li	a3, 0x10000
	li	a4, 2047
	li	a5, -2048
	sh1add	a0, a1, a2
	sh2add	a0, a1, a2
	sh3add	a0, a1, a2
	andn	a0, a1, a2
	orn	a0, a1, a2
	xnor	a0, a1, a2
	min	a0, a1, a2
	minu	a0, a1, a2
	max	a0, a1, a2
	maxu	a0, a1, a2
	rol	a0, a1, a2
	ror	a0, a1, a2
	rori	a0, a1, 5
	clz	a0, a1
	ctz	a0, a1
	cpop	a0, a1
	sext.b	a0, a1
	sext.h	a0, a1
	zext.h	a0, a1
	rev8	a0, a1
	orc.b	a0, a1
	bset	a0, a1, a2
	bclr	a0, a1, a2
	bext	a0, a1, a2
	binv	a0, a1, a2
	bseti	a0, a1, 5
	bclri	a0, a1, 5
	bexti	a0, a1, 5
	binvi	a0, a1, 5
	lr.w	a0, (a1)
	sc.w	a0, a1, (a2)
	amoadd.w	a0, a1, (a2)
	amoswap.w	a0, a1, (a2)
	amoxor.w	a0, a1, (a2)
	amoand.w	a0, a1, (a2)
	amoor.w	a0, a1, (a2)
	amomin.w	a0, a1, (a2)
	amomax.w	a0, a1, (a2)
	amominu.w	a0, a1, (a2)
	amomaxu.w	a0, a1, (a2)
	csrrw	a0, mstatus, a1
	csrrs	a0, fcsr, a1
	csrrc	a0, mie, a1
	csrrwi	a0, 0x340, 3
	csrrsi	a0, 0x341, 1
	csrrci	a0, 0x342, 2
	fence
	fence.i
