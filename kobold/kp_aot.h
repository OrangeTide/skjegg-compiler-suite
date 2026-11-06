/* kp_aot.h : the Kobold ahead-of-time emit pipeline.

   kp_aot_emit takes a parsed KPM module and writes native assembly for the
   linked backend, through the same instruction selector the JIT drives.  It is
   the stage after kpm_read, shared by skj-kp (which reads a module from a file)
   and the single-step skj-kpc (which builds the module in memory from the front
   end), so the two agree on entry renaming, export linkage, the guest-name
   remap, register allocation, and the overflow-trap policy. */

#ifndef KP_AOT_H
#define KP_AOT_H

#include "kpm.h"

#include <stdio.h>

/* Compile a parsed module to assembly on `out`.  Renames the entry routine for
   the C start file, gives exported symbols external linkage, applies the
   guest-name -> runtime-symbol remap, allocates registers, and emits with the
   overflow-trap policy Kobold wants.  Mutates the module in place.  Returns 0. */
int kp_aot_emit(struct kpm_module *m, FILE *out);

#endif /* KP_AOT_H */
