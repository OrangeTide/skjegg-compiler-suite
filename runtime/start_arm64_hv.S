/* start_arm64_hv.S : the ELF entry for the aarch64 hardware-virtualization
   guest.  The scheduler sets sp to the LP64 initial stack (argc, argv, envp,
   auxv) and the pc to _start.  Read argc/argv, call main, and exit with its
   return value through the syscall seam (syscall_arm64_hv.S). */

	.text
	.globl _start
_start:
	ldr	x0, [sp]		/* argc */
	add	x1, sp, #8		/* argv */
	bl	main
	bl	exit			/* w0 holds main's return */
.Lhang:
	b	.Lhang
