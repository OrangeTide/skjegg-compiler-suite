| sys.S : emulator-private TRAP #0 stubs for the ColdFire sandbox demo guests.
|
| The ColdFire C ABI passes arguments on the stack and returns in d0; each stub
| moves its two arguments into the syscall-argument registers d1/d2, sets the
| emulator-private selector in d0 and traps, and the result comes back in d0.
| These share the parent.c/child.c sources with the RISC-V and MIPS demos.

	.text

	| void *sys_map(unsigned size, unsigned flags);  -> d0 = base
	.globl sys_map
sys_map:
	move.l	4(%sp), %d1		| size  -> arg0
	move.l	8(%sp), %d2		| flags -> arg1
	move.l	#0x000f0001, %d0
	trap	#0
	rts

	| void sys_munmap(void *base, unsigned size);
	.globl sys_munmap
sys_munmap:
	move.l	4(%sp), %d1		| base  -> arg0
	move.l	8(%sp), %d2		| size  -> arg1
	move.l	#0x000f0002, %d0
	trap	#0
	rts

	| unsigned sys_spawn(const void *img, unsigned len);  -> d0 = handle
	.globl sys_spawn
sys_spawn:
	move.l	4(%sp), %d1		| img   -> arg0
	move.l	8(%sp), %d2		| len   -> arg1
	move.l	#0x000f0003, %d0
	trap	#0
	rts
