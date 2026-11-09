/* basic_sys_arm64.S - the psABI symbols BASIC needs that the shared AAPCS start
 * file (start_arm64_aapcs.S, written for cc + libc) does not provide: the moo
 * bump arena, and the time/random syscalls behind RND, TIME, and WAIT.  It is
 * linked alongside start_aapcs.o into every BASIC arm64 binary, the way
 * start_rv.S bundles the same seam for the rv32 build.
 *
 * Args arrive in the AAPCS registers (x0, x1, x2), which are also the Linux
 * syscall argument registers for these calls, so each wrapper only sets the
 * call number in x8 and issues svc #0.
 */

	.text

	.globl __moo_arena_alloc
	.globl __moo_arena_reset
	.globl clock_gettime
	.globl getrandom
	.globl nanosleep

/* void *__moo_arena_alloc(int size);  bump allocator, 8-byte aligned result. */
__moo_arena_alloc:
	mov	w0, w0			/* size, zero-extended into x0 */
	add	x0, x0, #7
	and	x0, x0, #-8		/* round the request up to 8 bytes */
	ldr	x1, =__moo_arena_ptr
	ldr	x2, [x1]		/* old pointer (return value) */
	add	x3, x2, x0
	str	x3, [x1]
	mov	x0, x2
	ret

/* void __moo_arena_reset(void); */
__moo_arena_reset:
	ldr	x1, =__moo_arena_ptr
	ldr	x2, =__moo_arena
	str	x2, [x1]
	ret

/* int clock_gettime(int clk_id, void *ts) - args already in x0/x1 */
clock_gettime:
	mov	x8, #113
	svc	#0
	ret

/* int getrandom(void *buf, int len, unsigned flags) - x0/x1/x2 */
getrandom:
	mov	x8, #278
	svc	#0
	ret

/* int nanosleep(const void *req, void *rem) - x0/x1 */
nanosleep:
	mov	x8, #101
	svc	#0
	ret

	.data
	.balign	8
__moo_arena_ptr:
	.quad	__moo_arena

	.bss
	.balign	8
__moo_arena:
	.space	131072
