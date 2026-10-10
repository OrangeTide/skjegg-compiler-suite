/* The skj-cc-rv half of the struct interop test.  It calls gcc-built functions
   that pass and return structs by value (a 2-word pt and a by-reference big),
   and it provides skj_cb, which gcc calls with a by-value struct.  Exercises
   both ABI directions for both the register and by-reference struct classes.
   Returns 42 on success. */

#include "rv_struct_interop.h"

int
skj_cb(struct pt p)
{
    return p.x * 10 + p.y;          /* gcc passes {5,6} -> 56 */
}

int
main(void)
{
    int r = 0;

    struct pt a = { 3, 4 };
    if (gcc_sum_pt(a) == 7) r += 1;                 /* skj -> gcc, <=8 arg */

    struct pt m = gcc_make_pt(8, 9);                /* gcc -> skj, <=8 return */
    if (m.x == 8 && m.y == 9) r += 2;

    struct big b = { 1, 2, 3, 4 };
    if (gcc_sum_big(b) == 10) r += 4;               /* skj -> gcc, by-ref arg */

    struct big mb = gcc_make_big(10);               /* by-ref return */
    if (mb.a == 10 && mb.b == 11 && mb.c == 12 && mb.d == 13) r += 8;

    if (gcc_calls_skj() == 56) r += 16;             /* gcc -> skj, <=8 arg */

    return (r == 31) ? 42 : r;
}
