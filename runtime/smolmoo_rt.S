| smolmoo_rt.S : ColdFire-legal runtime assembly for Excelsior on
| smolmoo (https://github.com/OrangeTide/smolmoo).
|
| The smolmoo build does not link start.S (no Linux syscalls, no _start:
| the binding's C _start enters at .text.entry, and the arena is C in
| exc_smolmoo.c), so only the source coroutine helpers live here. They
| are the __exc_src_next/__exc_src_yield pair from runtime/start.S
| (sources.md D4; keep the two in sync), rewritten with the movem forms
| real ColdFire has: no predecrement/postincrement movem, so a switch
| reserves its 72-byte save area with lea and stores through (d16,%sp).
| The saved-register memory layout is identical to start.S's, fp2-fp7 at
| 0..47 and d2-d7 at 48..71 from %sp up.
|
| struct exc_src (runtime/libexc.h wire format, allocated by
| __exc_src_new in libexc.c): 0 state (0 new, 1 running, 2 done),
| 4 body fn, 8 body sp, 12 body fp, 16 pump sp, 20 pump fp,
| 24 stack top, 28.. argument words.
|
| Made by a machine. PUBLIC DOMAIN (CC0-1.0)

    .text

| __exc_src_next(src) -> d0 = yielded (null-word maybe) value, 0 = done
    .globl __exc_src_next
__exc_src_next:
    move.l  4(%sp), %a0         | a0 = src
    move.l  (%a0), %d0
    subq.l  #2, %d0
    bne     1f
    move.l  #0, %d0             | exhausted stays exhausted
    rts
1:  | save the pump context on the pump stack
    lea     -72(%sp), %sp
    movem.l %d2-%d7, 48(%sp)
    fmove.d %fp2, (%sp)
    fmove.d %fp3, 8(%sp)
    fmove.d %fp4, 16(%sp)
    fmove.d %fp5, 24(%sp)
    fmove.d %fp6, 32(%sp)
    fmove.d %fp7, 40(%sp)
    move.l  %sp, 16(%a0)        | pump sp
    move.l  %fp, 20(%a0)        | pump fp
    move.l  (%a0), %d0
    subq.l  #1, %d0
    beq     2f
    | first pump: launch the body on the private stack
    move.l  #1, (%a0)           | state = running
    move.l  24(%a0), %sp
    move.l  %a0, -(%sp)         | the body's one argument: the source
    move.l  4(%a0), %a1
    jsr     (%a1)               | returns only when the body is exhausted
    move.l  (%sp), %a0          | the pushed argument (caller-pops ABI)
    move.l  #2, (%a0)           | state = done
    move.l  16(%a0), %sp        | back to the pump stack
    move.l  20(%a0), %fp
    bra     3f
2:  | resume the suspended body where its yield left off
    move.l  8(%a0), %sp
    move.l  12(%a0), %fp
    fmove.d (%sp), %fp2
    fmove.d 8(%sp), %fp3
    fmove.d 16(%sp), %fp4
    fmove.d 24(%sp), %fp5
    fmove.d 32(%sp), %fp6
    fmove.d 40(%sp), %fp7
    movem.l 48(%sp), %d2-%d7
    lea     72(%sp), %sp
    rts                          | into the body, after its yield call
3:  | exhausted: restore the pump context, return 0
    fmove.d (%sp), %fp2
    fmove.d 8(%sp), %fp3
    fmove.d 16(%sp), %fp4
    fmove.d 24(%sp), %fp5
    fmove.d 32(%sp), %fp6
    fmove.d 40(%sp), %fp7
    movem.l 48(%sp), %d2-%d7
    lea     72(%sp), %sp
    move.l  #0, %d0
    rts

| __exc_src_yield(src, v): suspend the body, return v from the pump.
| Never returns to its caller directly; a later __exc_src_next resumes
| right after the caller's jsr.
    .globl __exc_src_yield
__exc_src_yield:
    move.l  4(%sp), %a0         | a0 = src
    move.l  8(%sp), %d1         | d1 = the yielded value
    | save the body context on the body stack (mirrors the resume path)
    lea     -72(%sp), %sp
    movem.l %d2-%d7, 48(%sp)
    fmove.d %fp2, (%sp)
    fmove.d %fp3, 8(%sp)
    fmove.d %fp4, 16(%sp)
    fmove.d %fp5, 24(%sp)
    fmove.d %fp6, 32(%sp)
    fmove.d %fp7, 40(%sp)
    move.l  %sp, 8(%a0)         | body sp
    move.l  %fp, 12(%a0)        | body fp
    | switch to the pump and return the value from __exc_src_next
    move.l  16(%a0), %sp
    move.l  20(%a0), %fp
    fmove.d (%sp), %fp2
    fmove.d 8(%sp), %fp3
    fmove.d 16(%sp), %fp4
    fmove.d 24(%sp), %fp5
    fmove.d 32(%sp), %fp6
    fmove.d 40(%sp), %fp7
    movem.l 48(%sp), %d2-%d7
    lea     72(%sp), %sp
    move.l  %d1, %d0
    rts

    .section .note.GNU-stack, "", @progbits
