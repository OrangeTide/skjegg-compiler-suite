# Self-checking single-precision float (the F subset the rv32.c core runs).
# Computes (3.0 + 4.0) * 2.0 = 14.0 and checks the integer conversion, plus a
# float compare.  Exits 0 on success.
	.text
	.globl _start
_start:
	li	t0, 3
	fcvt.s.w	ft0, t0        # 3.0
	li	t0, 4
	fcvt.s.w	ft1, t0        # 4.0
	fadd.s	ft2, ft0, ft1          # 7.0
	li	t0, 2
	fcvt.s.w	ft3, t0        # 2.0
	fmul.s	ft2, ft2, ft3          # 14.0
	fcvt.w.s	t1, ft2, rtz   # 14
	li	t3, 14
	bne	t1, t3, .fail1

	# 3.0 < 4.0 must be true
	flt.s	t1, ft0, ft1
	li	t3, 1
	bne	t1, t3, .fail2

	# 4.0 <= 4.0 must be true
	fle.s	t1, ft1, ft1
	bne	t1, t3, .fail3

	# fabs/fneg round trip: |-7.0| back to 7
	fneg.s	ft4, ft2               # -14.0
	fabs.s	ft4, ft4               # 14.0
	fcvt.w.s	t1, ft4, rtz
	li	t3, 14
	bne	t1, t3, .fail4

	li	a0, 0
	j	.done
.fail1:	li a0, 1
	j .done
.fail2:	li a0, 2
	j .done
.fail3:	li a0, 3
	j .done
.fail4:	li a0, 4
.done:
	li	a7, 93
	ecall
