# Self-checking integer ops.  Each result is compared to a known constant;
# a mismatch exits with a distinct nonzero code, success exits 0.  Runs
# identically on qemu-riscv32 and skj-run.
	.text
	.globl _start
_start:
	li	s0, 20            # a
	li	s1, 6             # b

	add	t2, s0, s1
	li	t3, 26
	bne	t2, t3, .fail1
	sub	t2, s0, s1
	li	t3, 14
	bne	t2, t3, .fail2
	mul	t2, s0, s1
	li	t3, 120
	bne	t2, t3, .fail3
	div	t2, s0, s1
	li	t3, 3
	bne	t2, t3, .fail4
	rem	t2, s0, s1
	li	t3, 2
	bne	t2, t3, .fail5
	and	t2, s0, s1
	li	t3, 4
	bne	t2, t3, .fail6
	or	t2, s0, s1
	li	t3, 22
	bne	t2, t3, .fail7
	xor	t2, s0, s1
	li	t3, 18
	bne	t2, t3, .fail8
	slli	t2, s0, 2
	li	t3, 80
	bne	t2, t3, .fail9
	srli	t2, s0, 2
	li	t3, 5
	bne	t2, t3, .fail10
	slt	t2, s1, s0
	li	t3, 1
	bne	t2, t3, .fail11
	andi	t2, s0, 12
	li	t3, 4
	bne	t2, t3, .fail12
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
	j .done
.fail6:	li a0, 6
	j .done
.fail7:	li a0, 7
	j .done
.fail8:	li a0, 8
	j .done
.fail9:	li a0, 9
	j .done
.fail10: li a0, 10
	j .done
.fail11: li a0, 11
	j .done
.fail12: li a0, 12
.done:
	li	a7, 93
	ecall
