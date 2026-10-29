# Encoding fixture: the data-emitting directives, compared in .data.  Covers
# integer lists at each width, strings with and without a terminator, fill and
# alignment, and negative/hex values.
	.data
	.globl d
d:
	.word 0, 1, -1, 0x12345678
	.half 0, 2, -2, 0xabcd
	.byte 1, 2, 3, 255, -128
	.ascii "abc"
	.asciiz "hi"
	.string "xyz"
	.space 5
	.align 2
	.word 0x55667788
	.zero 3
	.byte 0x41
