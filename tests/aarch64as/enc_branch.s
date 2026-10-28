// branches: b/bl to external, cbz/cbnz, br/blr/ret, adr to local, svc, nop
.text
.globl br_test
br_test:
	bl	ext_func
	b	ext_tail
	cbz	w0, ext_tail
	cbnz	x1, ext_tail
	br	x0
	blr	x1
	adr	x19, .Llocal
	svc	#0
	nop
.Llocal:
	ret
