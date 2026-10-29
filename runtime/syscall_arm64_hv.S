/* syscall_arm64_hv.S : the Linux/aarch64 syscall seam for the hardware-
   virtualization guest.  AAPCS64 passes args in x0..x7; the aarch64 Linux
   syscall ABI passes args in x0..x5 with the number in x8.  For the calls here
   those are the same registers, so each wrapper sets x8 and reaches the host
   through the doorbell: an eight-byte load from SKJ_HV_DOORBELL (0x30000000),
   which faults out as MMIO.  The host reads x8 and x0..x5 and returns the
   result as the load's data, which the kernel places in x0.

   Same symbol set as syscall_rv.S and syscall_x86_64_hv.S, so the C libc and
   the Kobold runtime link against it unchanged.  write/read/open/close/lseek
   and exit are weak so the libc's overrides win; the __sys_* aliases are the
   raw entry points the fs/ backends call. */

	.text
	.weak	write
	.weak	read
	.weak	open
	.weak	close
	.weak	lseek
	.globl	__sys_write
	.globl	__sys_read
	.globl	__sys_open
	.globl	__sys_close
	.globl	__sys_lseek
	.globl	writev
	.globl	openat
	.globl	getdents64
	.globl	statx
	.globl	mkdirat
	.globl	unlinkat
	.globl	fchmodat
	.globl	renameat2
	.globl	clock_gettime
	.globl	getrandom
	.globl	nanosleep
	.globl	sched_yield
	.globl	__sys_brk
	.weak	exit
	.globl	_exit
	.globl	__skj_trap_backtrace
	.globl	__skj_p9_doorbell

/* ssize_t write(int fd, const void *buf, size_t n); */
write:
__sys_write:
	mov	x8, #64
	b	.Ldoorbell

/* ssize_t read(int fd, void *buf, size_t n); */
read:
__sys_read:
	mov	x8, #63
	b	.Ldoorbell

/* ssize_t writev(int fd, const struct iovec *iov, int iovcnt); */
writev:
	mov	x8, #66
	b	.Ldoorbell

/* int open(const char *path, int flags, int mode); aarch64 has no bare open,
   so this is openat(AT_FDCWD, path, flags, mode) with the args shifted up. */
open:
__sys_open:
	mov	x3, x2			/* mode */
	mov	x2, x1			/* flags */
	mov	x1, x0			/* path */
	mov	x0, #-100		/* AT_FDCWD */
	mov	x8, #56			/* openat */
	b	.Ldoorbell

/* int openat(int dirfd, const char *path, int flags, int mode); */
openat:
	mov	x8, #56
	b	.Ldoorbell

/* int close(int fd); */
close:
__sys_close:
	mov	x8, #57
	b	.Ldoorbell

/* off_t lseek(int fd, off_t offset, int whence); */
lseek:
__sys_lseek:
	mov	x8, #62
	b	.Ldoorbell

/* int getdents64(int fd, void *buf, int count); */
getdents64:
	mov	x8, #61
	b	.Ldoorbell

/* int statx(int dirfd, const char *path, int flags, unsigned mask, void *buf); */
statx:
	mov	x8, #291
	b	.Ldoorbell

/* int mkdirat(int dirfd, const char *path, int mode); */
mkdirat:
	mov	x8, #34
	b	.Ldoorbell

/* int unlinkat(int dirfd, const char *path, int flags); */
unlinkat:
	mov	x8, #35
	b	.Ldoorbell

/* int fchmodat(int dirfd, const char *path, int mode, int flags); */
fchmodat:
	mov	x8, #53
	b	.Ldoorbell

/* int renameat2(int oldfd, const char *old, int newfd, const char *new,
   unsigned flags); */
renameat2:
	mov	x8, #276
	b	.Ldoorbell

/* int clock_gettime(int clk_id, void *ts); */
clock_gettime:
	mov	x8, #113
	b	.Ldoorbell

/* int getrandom(void *buf, int len, unsigned flags); */
getrandom:
	mov	x8, #278
	b	.Ldoorbell

/* int nanosleep(const void *req, void *rem); */
nanosleep:
	mov	x8, #101
	b	.Ldoorbell

/* int sched_yield(void); */
sched_yield:
	mov	x8, #124
	b	.Ldoorbell

/* void *__sys_brk(void *addr); the raw Linux brk. */
__sys_brk:
	mov	x8, #214
	b	.Ldoorbell

/* long __skj_p9_doorbell(void *chan); SKJ_SYS_P9. */
__skj_p9_doorbell:
	ldr	x8, =0x000f0005
	b	.Ldoorbell

/* void __skj_trap_backtrace(int code); SKJ_SYS_FATAL. */
__skj_trap_backtrace:
	ldr	x8, =0x000f0000
	b	.Ldoorbell

/* The shared doorbell load: read eight bytes from SKJ_HV_DOORBELL; the host
   places the syscall result in x0.  Returns to the wrapper's caller. */
.Ldoorbell:
	ldr	x9, =0x30000000
	ldr	x0, [x9]
	ret

/* void exit(int status); (weak) and void _exit(int status). */
exit:
_exit:
	mov	x8, #93
	ldr	x9, =0x30000000
	ldr	x0, [x9]
.Lexitspin:
	b	.Lexitspin
