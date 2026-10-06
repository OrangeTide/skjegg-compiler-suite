/* Bit operations and the signed/unsigned boundary: shifts by constant and by
   variable amounts, the logical ops, and signed versus unsigned comparison and
   right shift, all of which the backends must lower identically. */

#include "torture.h"

int
main(void)
{
    unsigned u = 0xdeadbeefu;
    int s = -2000000000;
    int k;

    t_hex("and", u & 0x0f0f0f0fu);
    t_hex("or",  u | 0xf0f0f0f0u);
    t_hex("xor", u ^ 0xffffffffu);
    t_hex("not", (unsigned)~u);

    /* variable shift amounts, including the boundary of 31 */
    for (k = 0; k < 32; k += 7) {
        t_hex("ushl", (unsigned long long)(u << k));
        t_hex("ushr", (unsigned long long)(u >> k));
        t_sint("sshr", s >> k);          /* arithmetic (sign-filling) */
    }

    /* signed vs unsigned compare of the same bits */
    t_sint("slt", (s < 0));
    t_hex("ult", (unsigned long long)((unsigned)s < 100u));

    /* 64-bit variable shift across the word boundary */
    {
        unsigned long long w = 0x0123456789abcdefULL;
        int sh;
        for (sh = 0; sh < 64; sh += 13) {
            t_hex("w_shl", w << sh);
            t_hex("w_shr", w >> sh);
        }
    }
    return 0;
}
