/* sys.S : emulator-private ecall stubs for the sandbox demo guests.
 *
 * The RISC-V psABI already delivers the C arguments in a0, a1, ...; each stub
 * only sets a7 to the emulator-private call number and traps, and the result
 * comes back in a0. skj-cc-rv has no operand-constrained inline asm, so these
 * live here and are assembled by skj-as-rv.
 */

	.text

	/* void *sys_map(unsigned size, unsigned flags);  a0=size a1=flags -> a0=base */
	.globl sys_map
sys_map:
	li	a7, 0x000f0001
	ecall
	ret

	/* void sys_munmap(void *base, unsigned size);  a0=base a1=size */
	.globl sys_munmap
sys_munmap:
	li	a7, 0x000f0002
	ecall
	ret

	/* unsigned sys_spawn(const void *img, unsigned len);  a0=img a1=len -> a0=handle */
	.globl sys_spawn
sys_spawn:
	li	a7, 0x000f0003
	ecall
	ret
