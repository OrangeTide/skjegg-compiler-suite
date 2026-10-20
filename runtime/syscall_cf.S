| syscall_cf.S : the ColdFire/m68k Linux syscall wrappers, the one seam shared
| by the C crt (start.S) and the Kobold freestanding runtime (io_free_cf.S).
| Both .include this file, so the wrappers are written once.  Each is a trap #0:
| d0 is the syscall number, d1..d3 the arguments, the result in d0.  Arguments
| arrive on the stack (the toolkit/m68k C ABI); d2/d3 are callee-saved, so a
| wrapper that loads an argument into one saves and restores it.  exit is weak so
| libc's flushing exit overrides it; _exit is the raw process exit.

    .globl read
    .globl write
    .globl open
    .globl close
    .globl lseek
    .globl getdents64
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
