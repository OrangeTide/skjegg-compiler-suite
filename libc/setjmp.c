/* setjmp/longjmp for the WASM libc.
 *
 * WebAssembly has no addressable native stack, so a general setjmp (one that
 * returns a second time at a saved point) needs the stack-switching seam the
 * backend uses for delimited continuations.  skjegg does not need that here:
 * the only longjmp in the toolchain is the fatal-error unwind (ir/util.c die),
 * which runs its cleanup handlers before it jumps and then terminates.  So this
 * implementation covers exactly that pattern: setjmp installs a landing that is
 * never resumed, and longjmp ends the process with the passed value.
 *
 * A guest C program that uses setjmp/longjmp for ordinary (resumable) control
 * flow is out of scope and would need the continuation seam instead.
 */
#include <setjmp.h>
#include <stdlib.h>

int
setjmp(jmp_buf env)
{
    (void)env;
    return 0;
}

void
longjmp(jmp_buf env, int val)
{
    (void)env;
    exit(val ? val : 1);
}
