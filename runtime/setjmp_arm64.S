/* setjmp_arm64.S : AArch64 AAPCS setjmp/longjmp.  jmp_buf (a long[32]) holds
   the callee-saved registers longjmp must restore (x19-x28), the frame pointer
   and link register (x29/x30), and the stack pointer.  The callee-saved SIMD
   registers (d8-d15) are not saved: the compiler this libc serves keeps no live
   fp callee state across a setjmp.  Single str/ldr (not stp/ldp) keep to the
   in-tree assembler's instruction subset.  setjmp returns 0 on the direct call;
   longjmp resumes it returning val, or 1 if val is 0. */

	.text
	.globl setjmp
	.globl longjmp

setjmp:
	str	x19, [x0, #0]
	str	x20, [x0, #8]
	str	x21, [x0, #16]
	str	x22, [x0, #24]
	str	x23, [x0, #32]
	str	x24, [x0, #40]
	str	x25, [x0, #48]
	str	x26, [x0, #56]
	str	x27, [x0, #64]
	str	x28, [x0, #72]
	str	x29, [x0, #80]
	str	x30, [x0, #88]
	mov	x1, sp
	str	x1, [x0, #96]
	mov	w0, #0
	ret

longjmp:
	ldr	x19, [x0, #0]
	ldr	x20, [x0, #8]
	ldr	x21, [x0, #16]
	ldr	x22, [x0, #24]
	ldr	x23, [x0, #32]
	ldr	x24, [x0, #40]
	ldr	x25, [x0, #48]
	ldr	x26, [x0, #56]
	ldr	x27, [x0, #64]
	ldr	x28, [x0, #72]
	ldr	x29, [x0, #80]
	ldr	x30, [x0, #88]
	ldr	x2, [x0, #96]
	mov	sp, x2
	mov	w0, w1			/* val ... */
	cbnz	w0, 1f
	mov	w0, #1			/* ... but never 0 */
1:
	ret
