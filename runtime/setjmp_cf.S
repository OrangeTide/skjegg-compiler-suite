| setjmp_cf.S : ColdFire/m68k setjmp/longjmp.  jmp_buf (a long[32]) holds the
| callee-saved state longjmp must restore: the return PC, the stack pointer as
| the caller sees it after setjmp returns, d2-d7, and a6 (the frame pointer).
| The backend allocates only d2-d7 (int) and fp2-fp7 (float) and uses a6 for the
| frame, so a2-a5 are never live across a call and need not be saved.  Float is
| not saved: the compiler the self-host build runs does not keep live fp
| registers across a setjmp, the same assumption the RV32 twin (setjmp_rv.S)
| makes.  setjmp returns 0 on the direct call; longjmp resumes it returning val
| (or 1 if val is 0).  The toolkit/m68k C ABI passes arguments on the stack, so
| env is at 4(%sp) and val at 8(%sp).  skj-as has no movem with a displacement
| or a mixed register list, so each register moves on its own.

	.text
	.globl setjmp
	.globl longjmp

setjmp:
	move.l	4(%sp), %a0		| a0 = env
	move.l	(%sp), (%a0)		| env[0] = return PC
	lea	4(%sp), %a1		| sp as the caller sees it after rts
	move.l	%a1, 4(%a0)		| env[1] = restored SP
	move.l	%d2, 8(%a0)
	move.l	%d3, 12(%a0)
	move.l	%d4, 16(%a0)
	move.l	%d5, 20(%a0)
	move.l	%d6, 24(%a0)
	move.l	%d7, 28(%a0)
	move.l	%a6, 32(%a0)		| frame pointer
	clr.l	%d0			| direct call returns 0
	rts

longjmp:
	move.l	4(%sp), %a0		| a0 = env
	move.l	8(%sp), %d0		| d0 = val ...
	bne.s	1f
	moveq	#1, %d0			| ... but never 0
1:
	move.l	8(%a0), %d2
	move.l	12(%a0), %d3
	move.l	16(%a0), %d4
	move.l	20(%a0), %d5
	move.l	24(%a0), %d6
	move.l	28(%a0), %d7
	move.l	32(%a0), %a6		| frame pointer
	move.l	4(%a0), %sp		| restore SP
	move.l	(%a0), %a1		| a1 = saved PC
	jmp	(%a1)			| resume at setjmp's return, d0 = val
