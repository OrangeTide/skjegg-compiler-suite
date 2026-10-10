/* Integer promotions in expressions and across calls: char and short promote
   to int before arithmetic, an unsigned char or short promotes to signed int
   (int holds all its values), and a narrow argument is promoted when passed.
   A backend that computes in the narrow width, or promotes an unsigned sub-int
   to unsigned, would diverge from the host gcc oracle. */

#include "torture.h"

/* the parameters are int; narrow arguments promote at the call */
static int
widen3(int a, int b, int c)
{
    return a * 100 + b * 10 + c;
}

int
main(void)
{
    /* char arithmetic is done in int, so the intermediate does not wrap at 8
       bits even though each operand is a char */
    {
        signed char a = 100, b = 100;
        int r = a + b;              /* 200, not -56 */
        t_sint("char_add", r);      /* 200 */
    }

    /* an unsigned char promotes to signed int, so a comparison with a negative
       int is signed */
    {
        unsigned char uc = 200;
        int i = -1;
        t_sint("uc_gt", uc > i);    /* 1: 200 > -1 */
        t_sint("uc_val", uc + 0);   /* 200 */
    }

    /* a short multiply overflows int only, not short, and the product is taken
       in int */
    {
        short s = 300;
        int sq = s * s;             /* 90000 */
        t_sint("short_sq", sq);     /* 90000 */
    }

    /* unsigned short wraps at the int computation only when cast back */
    {
        unsigned short us = 60000;
        int sum = us + 10000;       /* 70000 in int */
        t_sint("us_sum", sum);      /* 70000 */
        unsigned short back = (unsigned short)(us + 10000); /* 70000 & 0xffff = 4464 */
        t_sint("us_wrap", back);    /* 4464 */
    }

    /* narrow values promoted as call arguments */
    {
        signed char a = 1;
        short b = 2;
        unsigned char c = 3;
        t_sint("call", widen3(a, b, c));   /* 123 */
    }

    /* a char compared and used as an index promotes cleanly */
    {
        char letters[4] = { 10, 20, 30, 40 };
        char idx = 2;
        t_sint("index", letters[idx] + letters[idx - 1]);  /* 30 + 20 = 50 */
    }

    /* the ternary's common type of char and int is int */
    {
        signed char c = -5;
        int r = (1 ? c : 0) * 2;    /* -10 */
        t_sint("tern", r);          /* -10 */
    }

    return 0;
}
