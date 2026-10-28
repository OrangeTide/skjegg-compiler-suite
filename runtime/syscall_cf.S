| syscall_cf.S : the ColdFire/m68k Linux syscall wrappers, the one seam shared
| by the C crt (start.S) and the Kobold freestanding runtime (io_free_cf.S).
| Both .include this file, so the wrappers are written once.  Each is a trap #0:
| d0 is the syscall number, d1..d3 the arguments, the result in d0.  Arguments
| arrive on the stack (the toolkit/m68k C ABI); d2/d3 are callee-saved, so a
| wrapper that loads an argument into one saves and restores it.  exit is weak so
| libc's flushing exit overrides it; _exit is the raw process exit.
|
| Error convention (the raw-seam ABI): a failing call returns a negative Linux
| errno in d0, passed through unchanged (lseek included), the same convention
| the rv32 seam and boris honor, so a guest reads one -errno space everywhere.

    | The five fd primitives are weak so the C libc's fs/ layer (libc/fdio.c)
    | can override them with path-resolving versions; a consumer that links no
    | fdio.c (the Kobold runtime, a bare crt) keeps these raw wrappers.  Their
    | __sys_* aliases are the raw entry points the fs/ backends call so they
    | reach the kernel without recursing through an override.
    .weak  read
    .weak  write
    .weak  open
    .weak  close
    .weak  lseek
    .globl __sys_read
    .globl __sys_write
    .globl writev
    .globl __sys_open
    .globl __sys_close
    .globl __sys_lseek
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

| int read(int fd, char *buf, int n);
read:
__sys_read:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | n
    move.l  #3, %d0             | __NR_read
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| int write(int fd, const char *buf, int n);
write:
__sys_write:
    movem.l %d2-%d3, -(%sp)     | save callee-saved regs
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | n
    move.l  #4, %d0             | __NR_write
    trap    #0
    movem.l (%sp)+, %d2-%d3     | restore
    rts

| int writev(int fd, const struct iovec *iov, int iovcnt);
writev:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | iov
    move.l  20(%sp), %d3        | iovcnt
    move.l  #146, %d0           | __NR_writev
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| int open(const char *path, int flags, int mode);
open:
__sys_open:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | path
    move.l  16(%sp), %d2        | flags
    move.l  20(%sp), %d3        | mode
    move.l  #5, %d0             | __NR_open
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| int openat(int dirfd, const char *path, int flags, int mode);
| The confined hostcap backend (fs/hostcap.c) issues this against a mount
| capability fd, so the raw dirfd matters; it is not interposed (no weak alias).
openat:
    movem.l %d2-%d5, -(%sp)
    move.l  20(%sp), %d1        | dirfd
    move.l  24(%sp), %d2        | path
    move.l  28(%sp), %d3        | flags
    move.l  32(%sp), %d4        | mode
    move.l  #288, %d0           | __NR_openat
    trap    #0
    movem.l (%sp)+, %d2-%d5
    rts

| int close(int fd);
close:
__sys_close:
    move.l  4(%sp), %d1         | fd
    move.l  #6, %d0             | __NR_close
    trap    #0
    rts

| long lseek(int fd, long offset, int whence);  (m68k has a simple 3-arg lseek)
lseek:
__sys_lseek:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | offset
    move.l  20(%sp), %d3        | whence
    move.l  #19, %d0            | __NR_lseek
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| int getdents64(int fd, void *buf, int count); reads directory entries into
| buf as a packed sequence of struct linux_dirent64.  Shared with the Kobold
| freestanding runtime (the dirs unit), hence here rather than in start.S.
getdents64:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | count
    move.l  #220, %d0           | __NR_getdents64
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| The file-metadata and path syscalls the libc wraps (libc/stat.c over statx,
| libc/fileops.c over the *at forms with AT_FDCWD).  m68k has no stat/fstat the
| libc can use the modern way, so the stat family goes through statx; mkdir /
| unlink / rename go through the *at forms.  Shared with the Kobold freestanding
| runtime (the files unit's Exists/FileSize/IsDir and Delete/Rename/MakeDir/
| RemoveDir), hence here rather than in start.S.  Stack arguments; d2-d5 are
| callee-saved, so save them around the trap.
statx:                             | statx(dirfd, path, flags, mask, buf)
    movem.l %d2-%d5, -(%sp)
    move.l  20(%sp), %d1           | dirfd
    move.l  24(%sp), %d2           | path
    move.l  28(%sp), %d3           | flags
    move.l  32(%sp), %d4           | mask
    move.l  36(%sp), %d5           | buf
    move.l  #379, %d0              | __NR_statx
    trap    #0
    movem.l (%sp)+, %d2-%d5
    rts

mkdirat:                           | mkdirat(dirfd, path, mode)
    movem.l %d2-%d5, -(%sp)
    move.l  20(%sp), %d1           | dirfd
    move.l  24(%sp), %d2           | path
    move.l  28(%sp), %d3           | mode
    move.l  #289, %d0              | __NR_mkdirat
    trap    #0
    movem.l (%sp)+, %d2-%d5
    rts

unlinkat:                          | unlinkat(dirfd, path, flags)
    movem.l %d2-%d5, -(%sp)
    move.l  20(%sp), %d1           | dirfd
    move.l  24(%sp), %d2           | path
    move.l  28(%sp), %d3           | flags
    move.l  #294, %d0              | __NR_unlinkat
    trap    #0
    movem.l (%sp)+, %d2-%d5
    rts

fchmodat:                          | fchmodat(dirfd, path, mode, flags)
    movem.l %d2-%d5, -(%sp)
    move.l  20(%sp), %d1           | dirfd
    move.l  24(%sp), %d2           | path
    move.l  28(%sp), %d3           | mode
    move.l  32(%sp), %d4           | flags
    move.l  #299, %d0              | __NR_fchmodat
    trap    #0
    movem.l (%sp)+, %d2-%d5
    rts

renameat2:                         | renameat2(olddirfd, oldpath, newdirfd, newpath, flags)
    movem.l %d2-%d5, -(%sp)
    move.l  20(%sp), %d1           | olddirfd
    move.l  24(%sp), %d2           | oldpath
    move.l  28(%sp), %d3           | newdirfd
    move.l  32(%sp), %d4           | newpath
    move.l  36(%sp), %d5           | flags
    move.l  #351, %d0              | __NR_renameat2
    trap    #0
    movem.l (%sp)+, %d2-%d5
    rts

| int clock_gettime(int clk_id, void *ts); writes a 32-bit {tv_sec, tv_nsec}
| through ts (the 32-bit timespec seam convention this seam and boris share).
| Args d1..d2.
clock_gettime:
    move.l  %d2, -(%sp)
    move.l  8(%sp), %d1         | clk_id
    move.l  12(%sp), %d2        | ts
    move.l  #263, %d0           | __NR_clock_gettime
    trap    #0
    move.l  (%sp)+, %d2
    rts

| int getrandom(void *buf, int len, unsigned flags); fills buf with len random
| bytes and returns the count.  Args d1..d3.
getrandom:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | buf
    move.l  16(%sp), %d2        | len
    move.l  20(%sp), %d3        | flags
    move.l  #352, %d0           | __NR_getrandom
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| int nanosleep(const void *req, void *rem); parks the task for the {tv_sec,
| tv_nsec} the req timespec holds.  Args d1..d2.  On boris the park/yield glue is
| bound to its sys_wait(timeout) host-call instead; this raw syscall is the
| skj-run backing.
nanosleep:
    move.l  %d2, -(%sp)
    move.l  8(%sp), %d1         | req
    move.l  12(%sp), %d2        | rem
    move.l  #162, %d0           | __NR_nanosleep
    trap    #0
    move.l  (%sp)+, %d2
    rts

| int sched_yield(void); gives up the rest of the task's turn without sleeping.
| No arguments.  A standard POSIX wrapper on the shared seam for C; the Kobold
| runtime does not wrap it (Sleep(0) is the guest-level yield for now).
sched_yield:
    move.l  #158, %d0           | __NR_sched_yield
    trap    #0
    rts

| void *__sys_brk(void *addr); the raw Linux brk (#45), returning the new
| program break in d0.  Both heaps grow through one brk-bump, libc/sbrk.c's
| __libc_sbrk: libc's malloc on the hosted ILP32 targets and the Kobold
| freestanding gheap (rt_free.c) call it alike, so it lives in the shared seam.
__sys_brk:
    move.l  4(%sp), %d1         | addr
    move.l  #45, %d0            | __NR_brk
    trap    #0
    rts

| void exit(int status); (weak) and void _exit(int status);
exit:
_exit:
    move.l  4(%sp), %d1
    move.l  #1, %d0             | __NR_exit
    trap    #0
1:  bra     1b

| void __skj_trap_backtrace(int code);
| Ask the host for a symbolized stack before a fatal exit: a TRAP #0 with the
| reserved CF_SYS_FATAL selector (0x000f0000) in %d0, outside the Linux/m68k
| syscall range.  It is the ColdFire twin of the RISC-V RV_SYS_FATAL ecall
| (boris VM_SYS_TRAP).  skj-run walks the a6 frame chain, symbolizes it, and
| returns, so the caller (rt_free.c's trap, C abort) falls through to its own
| exit.  A bare m68k kernel sees an unknown syscall and ignores it, so the path
| degrades to a plain exit off skj-run.
    .globl __skj_trap_backtrace
__skj_trap_backtrace:
    move.l  4(%sp), %d1        | exit code (informational; the caller exits)
    move.l  #0x000f0000, %d0   | CF_SYS_FATAL
    trap    #0
    rts
