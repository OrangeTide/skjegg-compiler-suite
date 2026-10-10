/* Integer division and modulo, signed and unsigned, 32-bit. The sign rules
   (C truncates toward zero, so the remainder takes the dividend's sign) and the
   signed/unsigned distinction are easy to lower wrong, and every backend must
   match the host gcc oracle. 64-bit divide is left out so the corpus needs no
   64-bit divide helper, per torture.h; INT_MIN / -1 is avoided (it is UB). */

#include "torture.h"

static void
pair(const char *q, const char *r, int a, int b)
{
    t_sint(q, a / b);
    t_sint(r, a % b);
}

int
main(void)
{
    /* all four sign combinations of truncating division */
    pair("q_pp", "r_pp",  17,  5);   /*  3,  2 */
    pair("q_np", "r_np", -17,  5);   /* -3, -2 */
    pair("q_pn", "r_pn",  17, -5);   /* -3,  2 */
    pair("q_nn", "r_nn", -17, -5);   /*  3, -2 */

    /* unsigned division of a value whose signed reading is negative */
    {
        unsigned a = 0x80000000u;    /* 2147483648 */
        unsigned b = 7u;
        t_hex("u_div", (unsigned long long)(a / b));
        t_hex("u_mod", (unsigned long long)(a % b));
    }

    /* division by a power of two differs in lowering (shift) from a general
       divisor, for both signs */
    t_sint("pow2_p",  1000 / 8);     /*  125 */
    t_sint("pow2_n", -1000 / 8);     /* -125 (toward zero) */
    t_sint("pow2_rn", -1000 % 8);    /* -1000 - (-125*8) = 0 */
    t_sint("pow2_r2", -1001 % 8);    /* -1 */

    /* a variable divisor, so the backend cannot fold it */
    {
        int d = 3;
        int acc = 0;
        for (int i = -6; i <= 6; i++)
            acc += i / d + i % d;
        t_sint("var_div", acc);
    }

    return 0;
}
