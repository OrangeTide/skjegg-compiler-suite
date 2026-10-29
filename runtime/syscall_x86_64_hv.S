/* syscall_x86_64_hv.S : the Linux/x86-64 syscall seam for the hardware-
   virtualization guest.  Each wrapper marshals the System V argument registers
   into the Linux syscall registers and reaches the host through the doorbell: an
   eight-byte load from SKJ_HV_DOORBELL (0x30000000), which faults out as MMIO.
   The host reads the syscall number and arguments from the registers and returns
   the result as the load's data, which the kernel places in rax.

   System V passes arg0..arg5 in rdi,rsi,rdx,rcx,r8,r9; the Linux syscall ABI
   wants rax=nr and arg0..arg5 in rdi,rsi,rdx,r10,r8,r9.  Only arg3 moves
   (rcx -> r10); the rest are already in place, so each wrapper is that one move,
   the number into rax, and the doorbell load.

   This is the same symbol set as syscall_rv.S, so the C libc fs/ layer and the
   Kobold freestanding runtime link against it unchanged.  write/read/open/close/
   lseek and exit are weak so the libc's path-resolving and flushing overrides
   win; the __sys_* aliases are the raw entry points the fs/ backends call. */

    .intel_syntax noprefix
    .text

    .weak  write
    .weak  read
    .weak  open
    .weak  close
    .weak  lseek
    .globl __sys_write
    .globl __sys_read
    .globl __sys_open
    .globl __sys_close
    .globl __sys_lseek
    .globl writev
    .globl openat
    .globl getdents64
    .globl statx
    .globl mkdirat
    .globl unlinkat
    .globl fchmodat
    .globl renameat2
    .globl clock_gettime
    .globl getrandom
    .globl nanosleep
    .globl sched_yield
    .globl __sys_brk
    .weak  exit
    .globl _exit
    .globl __skj_trap_backtrace
    .globl __skj_p9_doorbell

/* int write(int fd, const void *buf, int n); */
write:
__sys_write:
    mov r10, rcx
    mov rax, 1
    mov rax, [0x30000000]
    ret

/* int read(int fd, void *buf, int n); */
read:
__sys_read:
    mov r10, rcx
    mov rax, 0
    mov rax, [0x30000000]
    ret

/* int writev(int fd, const struct iovec *iov, int iovcnt); */
writev:
    mov r10, rcx
    mov rax, 20
    mov rax, [0x30000000]
    ret

/* int open(const char *path, int flags, int mode);
   x86-64 Linux keeps a bare open at nr=2; args already in rdi,rsi,rdx. */
open:
__sys_open:
    mov r10, rcx
    mov rax, 2
    mov rax, [0x30000000]
    ret

/* int openat(int dirfd, const char *path, int flags, int mode); */
openat:
    mov r10, rcx            /* arg3 = mode */
    mov rax, 257
    mov rax, [0x30000000]
    ret

/* int close(int fd); */
close:
__sys_close:
    mov r10, rcx
    mov rax, 3
    mov rax, [0x30000000]
    ret

/* long lseek(int fd, long offset, int whence); x86-64 has a native 64-bit
   lseek (nr=8), args already in rdi,rsi,rdx. */
lseek:
__sys_lseek:
    mov r10, rcx
    mov rax, 8
    mov rax, [0x30000000]
    ret

/* int getdents64(int fd, void *buf, int count); */
getdents64:
    mov r10, rcx
    mov rax, 217
    mov rax, [0x30000000]
    ret

/* int statx(int dirfd, const char *path, int flags, unsigned mask, void *buf); */
statx:
    mov r10, rcx            /* arg3 = mask */
    mov rax, 332
    mov rax, [0x30000000]
    ret

/* int mkdirat(int dirfd, const char *path, int mode); */
mkdirat:
    mov r10, rcx
    mov rax, 258
    mov rax, [0x30000000]
    ret

/* int unlinkat(int dirfd, const char *path, int flags); */
unlinkat:
    mov r10, rcx
    mov rax, 263
    mov rax, [0x30000000]
    ret

/* int fchmodat(int dirfd, const char *path, int mode, int flags); */
fchmodat:
    mov r10, rcx            /* arg3 = flags */
    mov rax, 268
    mov rax, [0x30000000]
    ret

/* int renameat2(int oldfd, const char *old, int newfd, const char *new,
   unsigned flags); */
renameat2:
    mov r10, rcx            /* arg3 = new path */
    mov rax, 316
    mov rax, [0x30000000]
    ret

/* int clock_gettime(int clk_id, void *ts); */
clock_gettime:
    mov r10, rcx
    mov rax, 228
    mov rax, [0x30000000]
    ret

/* int getrandom(void *buf, int len, unsigned flags); */
getrandom:
    mov r10, rcx
    mov rax, 318
    mov rax, [0x30000000]
    ret

/* int nanosleep(const void *req, void *rem); */
nanosleep:
    mov r10, rcx
    mov rax, 35
    mov rax, [0x30000000]
    ret

/* int sched_yield(void); */
sched_yield:
    mov rax, 24
    mov rax, [0x30000000]
    ret

/* void *__sys_brk(void *addr); the raw Linux brk (nr=12). */
__sys_brk:
    mov r10, rcx
    mov rax, 12
    mov rax, [0x30000000]
    ret

/* void exit(int status); (weak) and void _exit(int status). */
exit:
_exit:
    mov rax, 60
    mov rax, [0x30000000]
.Lexitspin:
    jmp .Lexitspin

/* void __skj_trap_backtrace(int code); SKJ_SYS_FATAL. */
__skj_trap_backtrace:
    mov rax, 0x000f0000
    mov rax, [0x30000000]
    ret

/* long __skj_p9_doorbell(void *chan); SKJ_SYS_P9. */
__skj_p9_doorbell:
    mov rax, 0x000f0005
    mov rax, [0x30000000]
    ret
