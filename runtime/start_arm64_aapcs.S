/*
 * start_arm64_aapcs.S : freestanding AArch64 _start and syscall wrappers for
 * skj-cc-arm64 under the AAPCS64 calling convention (cc P4).
 *
 * AAPCS64 passes function args in x0..x7; the AArch64 Linux syscall ABI passes
 * args in x0..x5 with the number in x8.  For read/write/exit those are the
 * same registers, so each wrapper just sets x8 and issues svc.  _start switches
 * to a low-memory stack (matching the runtime's arena placement) and calls
 * main, then passes main's return value to exit_group.
 *
 */
	.text
	.globl _start
	.globl write
	.globl read
	.globl open
	.globl close
	.globl lseek
	.globl statx
	.globl mkdirat
	.globl unlinkat
	.globl renameat2
	.globl fchmodat
	.globl getdents64
	.weak  exit
	.globl _exit
	.weak  __libc_fini

_start:
	ldr	x0, =stack_top
	mov	sp, x0			/* switch to the low-memory stack */
	bl	main
	mov	w19, w0			/* preserve main's return across the finalizer */
	ldr	x9, =__libc_fini	/* weak: 0 when no libc is linked */
	cbz	x9, 1f
	blr	x9			/* run atexit handlers and flush stdio */
1:
	mov	w0, w19			/* main's return is the exit status */
	mov	x8, #93
	svc	#0
2:	b	2b

/* ssize_t write(int fd, const void *buf, size_t n); AAPCS64 args already in
   x0/x1/x2, which are the syscall arg registers too */
write:
	mov	x8, #64
	svc	#0
	ret

/* ssize_t read(int fd, void *buf, size_t n) */
read:
	mov	x8, #63
	svc	#0
	ret

/* int open(const char *path, int flags, int mode);
   arm64 has no bare open, so this is openat(AT_FDCWD, path, flags, mode) with
   the arguments shifted up one register. */
open:
	mov	x3, x2			/* mode */
	mov	x2, x1			/* flags */
	mov	x1, x0			/* path */
	mov	x0, #-100		/* AT_FDCWD */
	mov	x8, #56			/* openat */
	svc	#0
	ret

/* int close(int fd); */
close:
	mov	x8, #57
	svc	#0
	ret

/* off_t lseek(int fd, off_t offset, int whence); arm64 has a simple lseek */
lseek:
	mov	x8, #62
	svc	#0
	ret

/* int statx(int dirfd, const char *path, int flags, unsigned mask, void *buf);
   args already in x0..x4 */
statx:
	mov	x8, #291
	svc	#0
	ret

/* The directory/path file operations the libc wraps with AT_FDCWD
   (libc/fileops.c): mkdirat, unlinkat (unlink and rmdir), renameat2.  arm64 has
   no bare mkdir/unlink/rename, so these *at forms are the primitives.  Args are
   already in x0.. on the AAPCS. */
mkdirat:
	mov	x8, #34
	svc	#0
	ret

unlinkat:
	mov	x8, #35
	svc	#0
	ret

renameat2:
	mov	x8, #276
	svc	#0
	ret

/* int fchmodat(int dirfd, const char *path, int mode, int flags); the libc
   chmod (libc/fileops.c) over AT_FDCWD.  Args are already in x0.. */
fchmodat:
	mov	x8, #53
	svc	#0
	ret

/* int getdents64(int fd, void *buf, int count); directory reads for the libc
   opendir/readdir (libc/dirent.c). */
getdents64:
	mov	x8, #61
	svc	#0
	ret

/* void exit(int status); status already in x0 */
exit:
_exit:
	mov	x8, #93
	svc	#0
1:	b	1b

	.bss
	.balign	16
	.space	131072
stack_top:
