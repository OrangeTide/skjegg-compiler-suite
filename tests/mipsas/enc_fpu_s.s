# Encoding fixture: single-precision COP1 and the remaining conversions and
# compares, complementing enc_base's double-precision COP1 block.
	.set noreorder
	.text
	.globl f
f:
	add.s $f0, $f2, $f4
	sub.s $f6, $f8, $f10
	mul.d $f12, $f14, $f16
	div.s $f18, $f20, $f22
	mov.s $f24, $f26
	neg.s $f28, $f30
	abs.s $f0, $f2
	cvt.s.w $f4, $f6
	cvt.w.s $f8, $f10
	cvt.d.w $f12, $f14
	c.eq.s $f20, $f22
	c.lt.s $f24, $f26
	c.le.s $f28, $f30
