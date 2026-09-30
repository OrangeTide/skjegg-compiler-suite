/* exc_coro_arm64.S : the source coroutine context switch for the arm64 JIT.

   An Excelsior `source of T` is a stackful coroutine (sources.md D4).  The
   constructor (__exc_src_new in libexc, plus compiler-emitted ctor code)
   allocates the source struct and a private stack from the arena; these two
   helpers switch between the pump (the `for` loop or `next(s)` caller) and the
   suspended body.  They are the AArch64 counterpart of runtime/start_rv.S's
   pair, and run as host code in the JIT: the pump side is reached on the JIT's
   low execution stack and the body runs on its own low arena stack, so every
   sp and fp is a 32-bit address stored at guest width in the struct.

   The struct (runtime/libexc.c, offsets shared with excelsior/lower.c):
       0  state      0 new, 1 running, 2 done
       4  body       the compiled body function
       8  body_sp    the suspended body's sp
      12  body_fp    the suspended body's fp
      16  pump_sp    the pump side, across a switch
      20  pump_fp
      24  stack_top  the private stack's high end
      28  arg0...    the body's argument words

   save_ctx / restore_ctx bank the callee-saved registers (x30 and the guest
   register pools x19-x28 and d8-d15) on the current stack around a switch; sp
   and fp travel through the struct, so each side resumes with its own frame. */

    .macro save_ctx
    sub sp, sp, #160
    str x30, [sp, #0]
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8,  d9,  [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    .endm

    .macro restore_ctx
    ldr x30, [sp, #0]
    ldp x19, x20, [sp, #16]
    ldp x21, x22, [sp, #32]
    ldp x23, x24, [sp, #48]
    ldp x25, x26, [sp, #64]
    ldp x27, x28, [sp, #80]
    ldp d8,  d9,  [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    add sp, sp, #160
    .endm

    .text

/* word __exc_src_next(struct exc_src *s):
 *   Resume or start the body; return the yielded value, or 0 when exhausted. */
    .globl __exc_src_next
    .type __exc_src_next, %function
__exc_src_next:
    ldr w9, [x0]                /* state */
    cmp w9, #2
    b.eq .Lsrc_done             /* exhausted stays exhausted */

    save_ctx
    mov x9, sp
    str w9, [x0, #16]           /* pump sp */
    str w29, [x0, #20]          /* pump fp */

    ldr w9, [x0]
    cbnz w9, .Lsrc_resume

    /* first pump: start the body on the top of its own stack */
    mov w9, #1
    str w9, [x0]                /* state = running */
    ldr w9, [x0, #24]           /* stack top */
    bic x9, x9, #15
    mov sp, x9
    mov x29, x9                 /* fp = sp */
    ldr w10, [x0, #4]           /* body function */
    mov x19, x0                 /* the body reads its source in x0; keep a copy */
    blr x10                     /* body(x0 = source) */

    /* the body returned: exhausted */
    mov x0, x19
    mov w9, #2
    str w9, [x0]                /* state = done */
    ldr w9, [x0, #16]
    mov sp, x9
    ldr w9, [x0, #20]
    mov x29, x9
    restore_ctx
    mov x0, #0
    ret

.Lsrc_resume:
    ldr w9, [x0, #8]            /* body sp */
    mov sp, x9
    ldr w9, [x0, #12]           /* body fp */
    mov x29, x9
    restore_ctx
    /* returns into __exc_src_yield's switch point */
    ret

.Lsrc_done:
    mov x0, #0
    ret
    .size __exc_src_next, .-__exc_src_next

/* void __exc_src_yield(struct exc_src *s, word value):
 *   Produce a value: park the body and return to the pump. */
    .globl __exc_src_yield
    .type __exc_src_yield, %function
__exc_src_yield:
    save_ctx
    mov x9, sp
    str w9, [x0, #8]            /* body sp */
    str w29, [x0, #12]          /* body fp */
    ldr w9, [x0, #16]           /* pump sp */
    mov sp, x9
    ldr w9, [x0, #20]           /* pump fp */
    mov x29, x9
    mov x0, x1                  /* the pump's result */
    restore_ctx
    ret
    .size __exc_src_yield, .-__exc_src_yield

    /* Mark the stack non-executable so the linker does not warn. */
    .section .note.GNU-stack,"",%progbits
