/* The gcc-built half of the skj-cc-rv struct interop test.  These functions
   receive and return structs by value through the standard RISC-V ilp32 psABI;
   skj-cc-rv output must reach them through that same ABI, in both directions.
   gcc_calls_skj closes the loop by calling the skj-built skj_cb with a struct. */

#include "rv_struct_interop.h"

int
gcc_sum_pt(struct pt p)
{
    return p.x + p.y;
}

struct pt
gcc_make_pt(int a, int b)
{
    struct pt r;
    r.x = a;
    r.y = b;
    return r;
}

int
gcc_sum_big(struct big b)
{
    return b.a + b.b + b.c + b.d;
}

struct big
gcc_make_big(int base)
{
    struct big r;
    r.a = base;
    r.b = base + 1;
    r.c = base + 2;
    r.d = base + 3;
    return r;
}

int
gcc_calls_skj(void)
{
    struct pt p;
    p.x = 5;
    p.y = 6;
    return skj_cb(p);
}
