// loads/stores: scaled, unscaled (ldur/stur), byte/half, signed, fp
.text
.globl ldst
ldst:
	ldr	x0, [x1]
	ldr	w0, [x1, #8]
	ldr	x0, [x29, #-16]
	str	x0, [sp, #16]
	str	w0, [x1, #4]
	ldur	w0, [x29, #-4]
	stur	x0, [sp, #-8]
	ldrb	w0, [x1]
	ldrsb	w0, [x1, #2]
	ldrh	w0, [x1, #4]
	ldrsh	w0, [x1]
	strb	w0, [x1]
	strh	w0, [x1, #6]
	ldr	d0, [x1]
	ldr	d0, [x29, #-16]
	str	d0, [sp, #8]
	ldr	s0, [x1, #4]
	str	s0, [x1]
	ret
