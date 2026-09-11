# Encoding fixture: the macros the backend leans on, expanded to match GNU as.
# .set noreorder so the comparison is the macro expansion alone, with no
# delay-slot scheduling.  The div/rem forms carry the full trap sequences.
	.set noreorder
	.text
	.globl t
t:
	move $t0, $t1
	li $t0, 0
	li $t1, 5
	li $t2, 65535
	li $t3, -1
	li $t4, -32768
	li $t5, 65536
	li $t6, 0x12345678
	li $t7, 0x12340000
	negu $t0, $t1
	neg $t8, $t9
	not $s0, $s1
	la $s2, gv
	mul $t0, $t1, $t2
	div $t0, $t1, $t2
	divu $t3, $t4, $t5
	rem $t6, $t7, $t8
	remu $s0, $s1, $s2
	l.d $f4, 8($sp)
	s.d $f6, 16($sp)
	l.s $f2, 4($sp)
	s.s $f8, 12($sp)
	jr $ra
	nop
	.data
	.globl gv
gv:
	.word 0
