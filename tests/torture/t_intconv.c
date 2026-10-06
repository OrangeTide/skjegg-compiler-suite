/* Integer promotions and the usual arithmetic conversions (C 6.3.1.1 and
   6.3.1.8). Each case is one the result's signedness or width depends on, so a
   wrong common type shows as a divergence from the host gcc oracle. The
   operands are fixed and chosen to avoid undefined behavior (no signed
   overflow, no shift past the width, no divide by zero, no INT_MIN / -1), so
   gcc's result is well defined and is the reference.

   The computation is the point, not the magnitude: a signed vs unsigned
   divide, modulo, right shift, or relational compare gives a different answer,
   and t_sint prints the 64-bit pattern so sign extension is visible. */

#include "torture.h"

int
main(void)
{
    /* A long long with an unsigned int: the common type is signed long long,
       since long long represents every unsigned int value, so the operation is
       signed. "either operand unsigned" would wrongly make it unsigned. A
       relational compare shows this without a divide (torture.h avoids division
       so the corpus needs no 64-bit divide helper). */
    {
        long long big = -1;
        unsigned u = 1;
        t_sint("ll_lt_u", big < u);         /* 1: signed, -1 < 1 */
    }

    /* A shift's type is the promoted left operand; the right operand's
       signedness is irrelevant, so a signed left shifts arithmetically even
       with an unsigned count. */
    {
        int x = -16;
        unsigned n = 2;
        t_sint("i_shr_un", x >> n);         /* -4, arithmetic */
        long long y = -256;
        unsigned m = 3;
        t_sint("ll_shr_un", y >> m);        /* -32, arithmetic */
    }

    /* A sub-int unsigned type promotes to signed int (int holds all its
       values), so an operation with it is signed, not unsigned. */
    {
        unsigned char c = 1;
        unsigned short s = 2;
        int i = -1;
        t_sint("uc_gt_i", c > i);           /* 1: 1 > -1 */
        t_sint("us_gt_i", s > i);           /* 1: 2 > -1 */
    }

    /* signed minus unsigned (equal rank) is unsigned int; widening it to long
       long zero-extends, so the high result is a large positive, not -1. */
    {
        int i = 0;
        unsigned u = 1;
        long long r = i - u;
        t_hex("i_sub_u_wide", (unsigned long long)r);   /* 0x00000000ffffffff */
    }

    /* equal-rank signed/unsigned: the signed operand converts to unsigned, so
       the compare is unsigned (this case was already correct; it guards
       against the fix flipping it). */
    {
        int i = -1;
        unsigned u = 1;
        t_sint("i_lt_u_eq", i < u);         /* 0: (unsigned)-1 < 1 is false */
    }

    /* sizeof applies the conversions, so the result type's width is visible at
       compile time: char + char is int (4), long long + int is long long (8). */
    t_sint("sz_cc", (long long)sizeof((char)1 + (char)1));      /* 4 */
    t_sint("sz_lli", (long long)sizeof((long long)1 + 1));      /* 8 */

    return 0;
}
