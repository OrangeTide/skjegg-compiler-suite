/* Loop forms and structured jumps: for, while, do-while, break and continue at
   several nesting depths, and a forward and a backward goto. The control flow
   is the point, so every backend must execute the same number of iterations
   and reach the same accumulator. */

#include "torture.h"

int
main(void)
{
    /* for with continue (skip multiples of 3) and break */
    {
        unsigned long long sum = 0;
        for (int i = 0; i < 100; i++) {
            if (i % 3 == 0)
                continue;
            if (i > 50)
                break;
            sum += (unsigned)i;
        }
        t_hex("for_cc", sum);
    }

    /* while counting down */
    {
        unsigned long long p = 1;
        int n = 10;
        while (n > 0) {
            p *= 2u;
            n--;
        }
        t_hex("while_pow", p);   /* 2^10 = 0x400 */
    }

    /* do-while runs its body at least once even with a false guard */
    {
        int runs = 0;
        int x = 0;
        do {
            runs++;
        } while (x);
        t_sint("dowhile", runs);  /* 1 */
    }

    /* nested loops with a break that leaves only the inner loop */
    {
        int hits = 0;
        for (int a = 0; a < 5; a++) {
            for (int b = 0; b < 5; b++) {
                if (b == a)
                    break;
                hits++;
            }
        }
        t_sint("nested_break", hits);  /* 0+1+2+3+4 = 10 */
    }

    /* goto: a forward skip and a backward loop */
    {
        int acc = 0;
        int i = 0;
    again:
        acc += i;
        if (i == 2)
            goto skip;
        acc += 100;
    skip:
        i++;
        if (i < 4)
            goto again;
        /* i=0: +0+100; i=1: +1+100; i=2: +2 (skip); i=3: +3+100 = 306 */
        t_sint("goto", acc);
    }

    return 0;
}
