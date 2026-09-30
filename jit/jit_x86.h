/* jit_x86.h : the x86-64 JIT, as embedded by a host or the skj-jit driver.

   The library face is the architecture-neutral jit_common.h (kp_jit, the
   kp_jit struct, kp_binding, kp_jit_entry/call/free, kp_call_guest).  The
   x86-64 target that satisfies it lives in jit_x86.c; this header is kept so an
   existing `#include "jit_x86.h"` still resolves to that API. */

#ifndef JIT_X86_H
#define JIT_X86_H

#include "jit_common.h"

#endif /* JIT_X86_H */
