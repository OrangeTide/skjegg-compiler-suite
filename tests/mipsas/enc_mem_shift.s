# Encoding fixture: shift edge cases (by 0 and 31, and nop as sll zero), plus
# the full load/store set with negative and boundary displacements.
	.set noreorder
	.text
	.globl f
f:
	sll $t0, $t1, 0
	sll $t2, $t3, 31
	srl $t4, $t5, 0
	sra $t6, $t7, 1
	lw $t0, -4($sp)
	lw $t1, 32767($t2)
	lw $t3, -32768($t4)
	sw $t5, -1($t6)
	lb $a0, -1($a1)
	lbu $a2, 127($a3)
	sh $s0, -2($s1)
