// add/sub reg + imm (incl sp extended form), mul/msub/div, shifts, logical, mov
.text
.globl arith
arith:
	add	w0, w1, w2
	add	x0, x1, x2
	sub	w3, w4, w5
	sub	x3, x4, x5
	add	x0, x1, #16
	sub	sp, sp, #32
	add	sp, sp, #32
	add	x29, sp, #16
	sub	sp, sp, x9
	add	sp, sp, x9
	cmp	w0, w1
	cmp	x0, x1
	neg	x0, x1
	mul	w0, w1, w2
	mul	x0, x1, x2
	msub	w0, w1, w2, w3
	sdiv	w0, w1, w2
	udiv	x0, x1, x2
	lsl	w0, w1, w2
	lsr	x0, x1, x2
	asr	w0, w1, w2
	and	w0, w1, w2
	orr	x0, x1, x2
	eor	w0, w1, w2
	bic	w0, w0, #7
	and	x0, x1, #0xff
	mvn	x0, x1
	mov	x0, x1
	mov	w0, w1
	mov	x0, #93
	mov	x0, #291
	mov	x0, #-100
	mov	sp, x0
	mov	x11, sp
	sxtw	x0, w1
	cset	w0, eq
	cset	x1, ne
	cset	w2, lt
	cset	w3, ge
	ret
