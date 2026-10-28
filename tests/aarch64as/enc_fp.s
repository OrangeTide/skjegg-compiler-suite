// fp arith, compare, conversions
.text
.globl fp
fp:
	fadd	d0, d1, d2
	fsub	d0, d1, d2
	fmul	d0, d1, d2
	fdiv	d0, d1, d2
	fneg	d0, d1
	fmov	d0, d1
	fmov	s0, s1
	fcmp	d0, d1
	fcvt	s0, d1
	fcvt	d0, s1
	fcvt	d0, h1
	fcvt	h0, d1
	scvtf	d0, w1
	scvtf	s0, w1
	fcvtzs	w0, d1
	fcvtzs	w0, s1
	ret
