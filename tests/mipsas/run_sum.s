# Run fixture: sum 1..9 in a loop, exit with the result (45).
# .set reorder, so skj-as-mips schedules the delay slots; a loop exercises
# branches, loads and stores.  Linux o32 exit syscall, run under qemu-mipsel.
	.set reorder
	.data
acc:	.word 0
	.text
	.globl __start
__start:
	addiu $t0, $zero, 0        # i
	addiu $t1, $zero, 0        # sum
	la $t3, acc
.Lloop:
	addiu $t2, $zero, 10
	slt $t4, $t0, $t2          # i < 10 ?
	beq $t4, $zero, .Ldone
	addu $t1, $t1, $t0         # sum += i
	sw $t1, 0($t3)             # spill through memory
	lw $t1, 0($t3)             # reload (load-delay exercise)
	addiu $t0, $t0, 1
	j .Lloop
.Ldone:
	move $a0, $t1              # exit code = sum(0..9) = 45
	addiu $v0, $zero, 4001
	syscall
