/* Calling-convention torture: more arguments than any backend has argument
   registers (so some pass on the stack), mixed int and 64-bit arguments (the
   register-pair rules), a 64-bit return, and a call whose target is computed
   between the arguments and the call. That last is the i64-pair against
   i32-single collision the coroutine seed first exposed: a temp defined after
   the arguments must not reuse an argument's register. */

#include "torture.h"

static int
sum8(int a, int b, int c, int d, int e, int f, int g, int h)
{
    return a + b*2 + c*3 + d*4 + e*5 + f*6 + g*7 + h*8;
}

static long long
mix(int a, long long b, int c, long long d, int e)
{
    return (long long)a + b - (long long)c + d + (long long)e;
}

static long long
wide(long long a, long long b, long long c)
{
    return a * 1000000LL + b * 1000LL + c;
}

int
main(void)
{
    t_sint("sum8", sum8(1, 2, 3, 4, 5, 6, 7, 8));
    t_sint("mix", mix(-5, 0x100000000LL, 7, 0x200000000LL, 9));
    t_hex("wide", (unsigned long long)wide(0x1122334455LL,
                                           0x66778899LL, 0xaabbLL));

    /* nested calls: the inner results are arguments to the outer, so the
       argument registers are live across a call */
    t_sint("nested", sum8(sum8(1, 1, 1, 1, 1, 1, 1, 1),
                          mix(1, 2, 3, 4, 5), 0, 0, 0, 0, 0, 0));
    return 0;
}
