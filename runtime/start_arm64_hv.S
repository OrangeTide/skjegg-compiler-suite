/* start_arm64_hv.S : the ELF entry for the aarch64 hardware-virtualization
   guest.  The scheduler sets sp to the LP64 initial stack (argc, argv, envp,
   auxv) and the pc to _start.  Read argc/argv, call main, and exit with its
   return value through the syscall seam (syscall_arm64_hv.S). */

	.text
	.globl _start
	.weak	environ			/* set below only when the libc links it */
_start:
	ldr	x0, [sp]		/* argc */
	add	x1, sp, #8		/* argv */
	lsl	x2, x0, #3		/* envp = argv + (argc + 1) slots */
	add	x2, x1, x2
	add	x2, x2, #8
	ldr	x3, =environ		/* &environ, 0 when the libc omits it (weak) */
	cbz	x3, 1f
	str	x2, [x3]
1:
	bl	main
	bl	exit			/* w0 holds main's return */
.Lhang:
	b	.Lhang
