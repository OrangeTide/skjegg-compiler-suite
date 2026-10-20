/* syscall_rv.S : the RV32 Linux syscall wrappers, the one seam shared by the C
   crt (start_rv.S) and the Kobold freestanding runtime (io_free_rv.S).  Both
   .include this file, so the wrappers are written once.  Each is a single ecall:
   a7 is the syscall number, a0..a4 the arguments (already in place on the ILP32
   psABI), the result in a0.  exit is weak so libc's flushing exit overrides it;
   _exit is the raw process exit. */

	.globl write
	.globl read
	.globl open
	.globl close
	.globl lseek
	.globl getdents64
	.weak  exit
	.globl _exit

/* int write(int fd, const char *buf, int n); */
	.type	write,@function
write:
	li	a7, 64
	ecall
	ret
	.size	write,.-write

/* int read(int fd, char *buf, int n); */
	.type	read,@function
read:
	li	a7, 63
	ecall
	ret
	.size	read,.-read

/* int open(const char *path, int flags, int mode);
   rv32 Linux has no bare open, so this is openat(AT_FDCWD, path, flags, mode)
   with the arguments shifted up one register. */
	.type	open,@function
open:
	mv	a3, a2			/* mode */
	mv	a2, a1			/* flags */
	mv	a1, a0			/* path */
	li	a0, -100		/* AT_FDCWD */
	li	a7, 56			/* openat */
	ecall
	ret
	.size	open,.-open

/* int close(int fd); */
	.type	close,@function
close:
	li	a7, 57
	ecall
	ret
	.size	close,.-close

/* long lseek(int fd, long offset, int whence);
   rv32 Linux has no simple lseek; syscall 62 is _llseek(fd, off_hi, off_lo,
   loff_t *result, whence) with a 64-bit offset split into two words and the new
   position written back through a pointer.  Sign-extend the 32-bit offset into
   the high word, pass a stack slot for the result, and return its low 32 bits
   (a file under 2 GiB, which every toolchain input is). */
	.type	lseek,@function
lseek:
	addi	sp, sp, -16
	mv	t0, a1			/* t0 = offset */
	mv	a4, a2			/* a4 = whence */
	srai	a1, t0, 31		/* a1 = off_hi (sign extension) */
	mv	a2, t0			/* a2 = off_lo */
	mv	a3, sp			/* a3 = &result (loff_t) */
	li	a7, 62			/* _llseek */
	ecall
	bltz	a0, 1f
	lw	a0, 0(sp)		/* low 32 bits of the new position */
	addi	sp, sp, 16
	ret
1:	li	a0, -1
	addi	sp, sp, 16
	ret
	.size	lseek,.-lseek

/* int getdents64(int fd, void *buf, int count); reads directory entries into
   buf as a packed sequence of struct linux_dirent64.  Shared with the Kobold
   freestanding runtime (the dirs unit), hence here rather than in start_rv.S.
   Args already in a0..a2. */
	.type	getdents64,@function
getdents64:
	li	a7, 61
	ecall
	ret
	.size	getdents64,.-getdents64

/* void exit(int status); (weak) and void _exit(int status); */
	.type	exit,@function
	.type	_exit,@function
exit:
_exit:
	li	a7, 93
	ecall
1:	j	1b
	.size	exit,.-exit
	.size	_exit,.-_exit

/* void __skj_trap_backtrace(int code);
   The skj-run private RV_SYS_FATAL ecall: print a symbolized backtrace for the
   calling frame and end the process with exit code a0, so an abort or a runtime
   trap shows a source-level stack like a CPU fault does.  a7 sits far above the
   Linux range, so on qemu-user it returns -ENOSYS and control returns here; the
   caller then falls through to its own exit, so this is a no-op off skj-run. */
	.globl	__skj_trap_backtrace
	.type	__skj_trap_backtrace,@function
__skj_trap_backtrace:
	li	a7, 0x000f0004		/* RV_SYS_FATAL */
	ecall
	ret
	.size	__skj_trap_backtrace,.-__skj_trap_backtrace
