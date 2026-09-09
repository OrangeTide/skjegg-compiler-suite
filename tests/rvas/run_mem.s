# Self-checking loads and stores through la to .data and .bss, exercising
# R_RISCV_PCREL_HI20/LO12 relocations and byte/half/word memory access.
# Exits 0 on success.
	.text
	.globl _start
_start:
	la	t0, buf
	li	t1, 0x41
	sb	t1, 0(t0)
	li	t1, 0x1234
	sh	t1, 2(t0)
	li	t1, 0x9abcdef0
	sw	t1, 4(t0)

	lbu	t2, 0(t0)
	li	t3, 0x41
	bne	t2, t3, .fail1
	lhu	t2, 2(t0)
	li	t3, 0x1234
	bne	t2, t3, .fail2
	lw	t2, 4(t0)
	li	t3, 0x9abcdef0
	bne	t2, t3, .fail3

	# signed byte load of 0x80 sign-extends to -128
	li	t1, 0x80
	sb	t1, 8(t0)
	lb	t2, 8(t0)
	li	t3, -128
	bne	t2, t3, .fail4

	# read an initialized word from .data
	la	t0, seed
	lw	t2, 0(t0)
	li	t3, 0xcafe
	bne	t2, t3, .fail5

	li	a0, 0
	j	.done
.fail1:	li a0, 1
	j .done
.fail2:	li a0, 2
	j .done
.fail3:	li a0, 3
	j .done
.fail4:	li a0, 4
	j .done
.fail5:	li a0, 5
.done:
	li	a7, 93
	ecall

	.data
seed:
	.word	0xcafe

	.bss
buf:
	.space	16
