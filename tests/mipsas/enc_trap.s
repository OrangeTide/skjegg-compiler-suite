# Encoding fixture: the trapping arithmetic forms (add/addi/sub) that overflow
# on signed overflow, distinct from the wrapping addu/subu/addiu the corpus
# already covers.  .set noreorder.
	.set noreorder
	.text
	.globl f
f:
	add $t0, $t1, $t2
	add $v0, $v0, $v1
	sub $s0, $s1, $s2
	addi $t0, $t1, 100
	addi $sp, $sp, -16
	addi $a0, $a1, -32768
	addi $t2, $t3, 32767
