/* co_switch_cf.S : ColdFire/m68k context switch for the coroutine runtime */

/* void co_switch(struct co_ctx *from, struct co_ctx *to)
 *   Toolkit convention: args on the stack, 4(sp) = from, 8(sp) = to on entry,
 *   with the return address at 0(sp). co_ctx.sp is at offset 0.
 * Save the callee-saved registers below the return address, stash sp into
 * *from, load sp from *to, restore its callee-saved registers, and rts into
 * whatever that stack was about to return to. The saved return address rides
 * the switched stack, so a plain call/rts carries control across the switch.
 * The callee-saved set is the backend's: d2-d7, the a2-a6 address registers
 * (a6 is the frame pointer, so it must be preserved across the switch), and
 * the callee-saved FPU registers fp2-fp7, since a suspended coroutine may hold
 * live floats. The 96-byte block is stored through (sp) in control mode (no
 * predecrement), the ColdFire-legal form; the return address sits just above
 * it at base+96, which the seed in coro.c fills with the trampoline. */

    .text
    .globl co_switch
co_switch:
    move.l  4(%sp), %a0         | from
    move.l  8(%sp), %a1         | to
    lea     -96(%sp), %sp       | reserve the save block; ret now at 96(sp)
    movem.l %d2-%d7, (%sp)      | d2-d7 at 0..23
    move.l  %a2, 24(%sp)
    move.l  %a3, 28(%sp)
    move.l  %a4, 32(%sp)
    move.l  %a5, 36(%sp)
    move.l  %a6, 40(%sp)
    fmove.d %fp2, 48(%sp)
    fmove.d %fp3, 56(%sp)
    fmove.d %fp4, 64(%sp)
    fmove.d %fp5, 72(%sp)
    fmove.d %fp6, 80(%sp)
    fmove.d %fp7, 88(%sp)

    move.l  %sp, (%a0)          | from->sp = sp (ret rides at base+96)
    move.l  (%a1), %sp          | sp = to->sp

    movem.l (%sp), %d2-%d7
    move.l  24(%sp), %a2
    move.l  28(%sp), %a3
    move.l  32(%sp), %a4
    move.l  36(%sp), %a5
    move.l  40(%sp), %a6
    fmove.d 48(%sp), %fp2
    fmove.d 56(%sp), %fp3
    fmove.d 64(%sp), %fp4
    fmove.d 72(%sp), %fp5
    fmove.d 80(%sp), %fp6
    fmove.d 88(%sp), %fp7
    lea     96(%sp), %sp        | sp -> ret slot (base+96)
    rts
