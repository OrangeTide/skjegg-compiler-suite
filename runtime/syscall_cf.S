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

    .globl read
    .globl write
    .globl open
    .globl close
    .globl lseek
    .globl getdents64
    .globl clock_gettime
    .globl getrandom
    .globl nanosleep
    .globl sched_yield
    .globl __sys_brk
    .weak  exit
    .globl _exit

| int read(int fd, char *buf, int n);
read:
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
    movem.l %d2-%d3, -(%sp)     | save callee-saved regs
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | n
    move.l  #4, %d0             | __NR_write
    trap    #0
    movem.l (%sp)+, %d2-%d3     | restore
    rts

| int open(const char *path, int flags, int mode);
open:
    movem.l %d2-%d3, -(%sp)
    move.l  12(%sp), %d1        | path
    move.l  16(%sp), %d2        | flags
    move.l  20(%sp), %d3        | mode
    move.l  #5, %d0             | __NR_open
    trap    #0
    movem.l (%sp)+, %d2-%d3
    rts

| int close(int fd);
close:
    move.l  4(%sp), %d1         | fd
    move.l  #6, %d0             | __NR_close
    trap    #0
    rts

| long lseek(int fd, long offset, int whence);  (m68k has a simple 3-arg lseek)
lseek:
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
| program break in d0.  Both heaps grow through it: libc/sbrk.c on the hosted
| ILP32 targets and the Kobold freestanding gheap (rt_free.c), so it lives in
| the shared seam.
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
| ColdFire has no skj-run backtrace facility yet (that is RISC-V only), so this
| is a no-op; a fatal path calls it then falls through to its own exit.  It
| exists so rt_free.c (shared with the RISC-V tier, where the crt provides the
| real RV_SYS_FATAL ecall) links the same on both targets.
    .globl __skj_trap_backtrace
__skj_trap_backtrace:
    rts
