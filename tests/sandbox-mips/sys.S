/* sys.S : emulator-private syscall stubs for the MIPS sandbox demo guests.
 *
 * The o32 ABI already delivers the C arguments in $a0, $a1, ...; each stub only
 * sets $v0 to the emulator-private call number and traps, and the result comes
 * back in $v0.  skj-cc-mips has operand-constrained inline asm, but these live
 * here to share the parent.c/child.c sources with the RISC-V sandbox demo.
 */

	.text
	.set noreorder

	/* void *sys_map(unsigned size, unsigned flags);  a0=size a1=flags -> v0=base */
	.globl sys_map
sys_map:
	li	$v0, 0x000f0001
	syscall
	jr	$ra
	nop

	/* void sys_munmap(void *base, unsigned size);  a0=base a1=size */
	.globl sys_munmap
sys_munmap:
	li	$v0, 0x000f0002
	syscall
	jr	$ra
	nop

	/* unsigned sys_spawn(const void *img, unsigned len);  a0=img a1=len -> v0=handle */
	.globl sys_spawn
sys_spawn:
	li	$v0, 0x000f0003
	syscall
	jr	$ra
	nop
