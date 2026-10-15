/* io_free_cf.S : ColdFire host-seam syscall wrappers for the freestanding guest.
 *
 * The seam io_free.c builds on (kp_sys_write / read / exit / open / close) is one
 * trap #0 each: d0 is the syscall number, d1..d3 the arguments, the result comes
 * back in d0 (runtime/start.S documents the same convention; skj-run's CFV4e user
 * mode services the trap).  Expressing that in C needs register-bound extended
 * inline asm, which a compiler without it cannot build, so the wrappers live here
 * and io_free.c is plain C.  The functions follow the m68k C ABI (arguments on the
 * stack, result in d0), which is the backend's toolkit convention, so a caller
 * built by skj-cc or by m68k gcc reaches them the same way.  d2 and d3 are
 * callee-saved, so a wrapper that loads an argument into one saves and restores it.
 */

    .text

    .globl kp_sys_write
kp_sys_write:
    lea     -8(%sp), %sp        | reserve a save slot; ret now at 8(sp)
    movem.l %d2-%d3, (%sp)      | preserve the callee-saved args we clobber
    moveq   #4, %d0             | SYS_write
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | len
    trap    #0
    movem.l (%sp), %d2-%d3
    lea     8(%sp), %sp
    rts

    .globl kp_sys_read
kp_sys_read:
    lea     -8(%sp), %sp
    movem.l %d2-%d3, (%sp)
    moveq   #3, %d0             | SYS_read
    move.l  12(%sp), %d1        | fd
    move.l  16(%sp), %d2        | buf
    move.l  20(%sp), %d3        | len
    trap    #0
    movem.l (%sp), %d2-%d3
    lea     8(%sp), %sp
    rts

    .globl kp_sys_open
kp_sys_open:
    lea     -8(%sp), %sp
    movem.l %d2-%d3, (%sp)
    moveq   #5, %d0             | SYS_open
    move.l  12(%sp), %d1        | path
    move.l  16(%sp), %d2        | flags
    move.l  20(%sp), %d3        | mode
    trap    #0
    movem.l (%sp), %d2-%d3
    lea     8(%sp), %sp
    rts

    .globl kp_sys_close
kp_sys_close:
    moveq   #6, %d0             | SYS_close
    move.l  4(%sp), %d1         | fd
    trap    #0
    rts

    .globl kp_sys_exit
kp_sys_exit:
    moveq   #1, %d0             | SYS_exit
    move.l  4(%sp), %d1         | code
    trap    #0
1:  bra     1b                  | never returns
