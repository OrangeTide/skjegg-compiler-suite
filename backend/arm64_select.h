/* arm64_select.h : the shared AArch64 (AAPCS64) instruction selector.

   One selector drives both the AOT text sink (backend/arm64_mc_text.c) and the
   JIT byte sink (jit/jit_arm64.c) through backend/arm64_mc.h.  The caller sets up
   the sink, then calls arm64_select_func for each function.  This is the AArch64
   parallel of backend/x86_select.h. */

#ifndef ARM64_SELECT_H
#define ARM64_SELECT_H

#include <stddef.h>

struct mc64;
struct ir_func;

/* How a divide by zero is handled.  AArch64's sdiv/udiv do not fault on a zero
   divisor (the result is 0), so the two sinks want different behavior: the JIT,
   where cc treats division by zero as an error, guards the divisor and calls a
   trap helper (TRAP_GUARDED); the AOT emits the bare divide (TRAP_HARDWARE),
   matching the historical arm64_emit.c output. */
enum arm64_trap_policy {
    ARM64_TRAP_HARDWARE = 0,
    ARM64_TRAP_GUARDED = 1,
};

/* Select machine code for one function, emitting through the sink m.  `tp` picks
   the divide trap policy.  Returns 0 on success; on an unsupported opcode fills
   err and returns -1.  A symbol the sink could not resolve is reported by the
   sink through its own error channel, which the caller checks after this
   returns. */
int arm64_select_func(struct mc64 *m, struct ir_func *fn, int tp,
                      char *err, size_t errlen);

#endif /* ARM64_SELECT_H */
