# spim load-delay fixture (SPIM syscall ABI, run under spim -delayed_loads).
#
# Written .set reorder with no hand-placed delay slots, so skj-as-mips's
# scheduler must insert the load-delay nop (between the lw and the addiu that
# reads it) and the branch-delay nop.  Run through spim's accurate R2000 model,
# where a missing load-delay nop reads stale data and the answer is wrong.
# Self-checking: exit2 (syscall 17) with the computed value, expected 42.
	.set reorder
	.data
v:	.word 41
	.text
	.globl main
main:
	la $t0, v
	lw $t1, 0($t0)             # load
	addiu $t1, $t1, 1          # uses $t1 in the next instruction: needs a nop
	addiu $t2, $zero, 42
	beq $t1, $t2, good         # branch delay slot must be filled
	addiu $a0, $zero, 1        # wrong path -> exit 1
	li $v0, 17
	syscall
good:
	addiu $a0, $zero, 42
	li $v0, 17
	syscall
