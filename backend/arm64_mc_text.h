/* arm64_mc_text.h : the GAS text sink for the shared AArch64 selector.

   Implements the backend/arm64_mc.h sink interface by writing AArch64 GAS
   assembly, the form the AOT compiler emits.  The driver creates a sink over an
   output stream and resets the per-function label namespace before each
   function.  The AArch64 parallel of backend/mc_text.h. */

#ifndef ARM64_MC_TEXT_H
#define ARM64_MC_TEXT_H

#include <stdio.h>

struct mc64;

struct mc64 *mct64_new(FILE *out);
void mct64_free(struct mc64 *m);
void mct64_begin_func(struct mc64 *m, int serial);
const char *mct64_errsym(struct mc64 *m);

#endif /* ARM64_MC_TEXT_H */
