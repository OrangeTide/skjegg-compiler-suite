/* call_guest_cf.S : call an AOT-compiled ColdFire guest entry on the execution stack */

/* uint64_t kp_call_guest(void *entry, void *stack_top,
 *                        const int64_t *iargs, const double *fargs, double *fret)
 *   Toolkit convention: arguments on the stack, so on entry
 *     4(sp)=entry  8(sp)=stack_top  12(sp)=iargs  16(sp)=fargs  20(sp)=fret
 *   with the return address at 0(sp).
 *
 * The guest entry is a Kobold program's entry: it takes no arguments (it reads
 * ParamStr / ParamCount through the runtime) and returns nothing, so iargs and
 * fargs are unused here (main passes them all zero). The job is only to run the
 * entry on the low execution stack main mapped, as the JIT driver does, then
 * restore the host stack. ColdFire is natively 32-bit, so there is no
 * address-width juggling; the switch keeps a deep self-compile off the host
 * stack, as kp_crt_cf.S does for the freestanding tier.
 *
 * The result is unused by main; clear *fret and return 0 in d0:d1. d2 and
 * a2-a4 are callee-saved (the backend's set), so the guest preserves the host
 * sp held in d2 and the fret pointer held in a4 across the call. */

    .text
    .globl kp_call_guest
kp_call_guest:
    lea     -16(%sp), %sp          | save the callee-saved registers we use
    movem.l %d2/%a2-%a4, (%sp)     | d2 at 0, a2 at 4, a3 at 8, a4 at 12
    move.l  20(%sp), %a2           | entry       (4(old sp))
    move.l  24(%sp), %a3           | stack_top   (8(old sp))
    move.l  36(%sp), %a4           | fret        (20(old sp))
    move.l  %sp, %d2               | save the host stack pointer
    move.l  %a3, %sp               | switch to the low execution stack
    jsr     (%a2)                  | run the guest entry (toolkit ABI, no args)
    move.l  %d2, %sp               | restore the host stack
    clr.l   (%a4)                  | *fret = 0 (double, 8 bytes)
    clr.l   4(%a4)
    movem.l (%sp), %d2/%a2-%a4     | restore callee-saved registers
    lea     16(%sp), %sp
    clr.l   %d0                    | result unused; return 0 in d0:d1
    clr.l   %d1
    rts
