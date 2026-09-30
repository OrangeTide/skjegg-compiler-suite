/* call_guest_arm64.S : call a JIT-compiled guest entry with marshaled arguments */

/* uint64_t kp_call_guest(void *entry, void *stack_top,
 *                        const int64_t iargs[6], const double fargs[8],
 *                        double *fret)
 *   entry = x0, stack_top = x1, iargs = x2, fargs = x3, fret = x4.
 *
 * The guest runs on the JIT's low-memory execution stack (its locals' addresses
 * must fit 32 bits), so switch sp to stack_top (already 16-byte aligned) for the
 * call and restore it after.  Load the AAPCS64 argument registers from the
 * arrays: all eight float registers and the first six integer registers, since
 * the guest reads only the ones its signature declares and ignores the rest.
 * The integer result comes back in x0; the float result is stored through fret. */

    .text
    .globl kp_call_guest
    .type kp_call_guest, %function
kp_call_guest:
    stp x29, x30, [sp, #-16]!
    stp x19, x20, [sp, #-16]!
    stp x21, x22, [sp, #-16]!
    stp x23, x24, [sp, #-16]!

    mov x19, x0             /* entry */
    mov x20, x1             /* stack_top */
    mov x21, x2             /* iargs */
    mov x22, x3             /* fargs */
    mov x23, x4             /* fret */
    mov x24, sp            /* save the host stack pointer */

    /* float arguments d0..d7 from fargs */
    ldp d0, d1, [x22, #0]
    ldp d2, d3, [x22, #16]
    ldp d4, d5, [x22, #32]
    ldp d6, d7, [x22, #48]

    /* integer arguments x0..x5 from iargs (read before switching) */
    ldp x0, x1, [x21, #0]
    ldp x2, x3, [x21, #16]
    ldp x4, x5, [x21, #32]

    mov sp, x20            /* switch to the low execution stack */
    blr x19                /* run the guest entry */
    mov sp, x24            /* restore the host stack */

    str d0, [x23]          /* hand back the float result */

    ldp x23, x24, [sp], #16
    ldp x21, x22, [sp], #16
    ldp x19, x20, [sp], #16
    ldp x29, x30, [sp], #16
    ret                     /* integer result already in x0 */
    .size kp_call_guest, .-kp_call_guest

    /* Mark the stack non-executable so the linker does not warn. */
    .section .note.GNU-stack,"",%progbits
