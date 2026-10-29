# Encoding fixture: the pseudo and direct branches/jumps not in enc_base:
# beqz/bnez (compare against $zero), the j/jal direct jumps, and the COP1
# condition branches bc1t/bc1f.  Targets are defined local labels so both
# assemblers resolve them directly.  .set noreorder.
	.set noreorder
	.text
	.globl f
f:
L0:
	beqz $t0, L0
	bnez $t1, L0
	bc1t L0
	bc1f L0
	j L0
	jal L0
	j f
	jal f
