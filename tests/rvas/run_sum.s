	.text
	.globl _start
_start:
	la	t0, count
	lw	a0, 0(t0)        # a0 = N
	li	t1, 0            # sum
	li	t2, 1            # i
.Lloop:
	bgt	t2, a0, .Ldone
	add	t1, t1, t2
	addi	t2, t2, 1
	j	.Lloop
.Ldone:
	mv	a0, t1
	call	addseven
	# exit(a0)
	li	a7, 93
	ecall
addseven:
	addi	a0, a0, 7
	ret
	.data
count:
	.word	10
