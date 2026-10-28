// data: byte/word/quad, symbol refs (ABS64/ABS32), strings, align, ldr= pool
.text
.globl pool
pool:
	ldr	w19, =0
	ldr	w20, =5
	ldr	x21, =4
	ldr	w19, =0
	ldr	x0, =gsym
	ldr	x1, =gsym
	.ltorg
	ret
.data
.globl d
d:
	.byte	1, 2, 3, 0xff
	.word	0x12345678
	.quad	0x1122334455667788
	.quad	gsym
	.align	3
.globl gsym
gsym:
	.quad	0
.section .rodata
msg:
	.string	"hello"
