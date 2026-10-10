/* Bit-fields: packing several fields into a storage unit, reading and writing
   them, signed versus unsigned extraction, a zero-width separator, and a field
   that forces a fresh unit. Only the read-back values are printed, so a correct
   layout makes every backend agree; the bit positions themselves may be
   target-defined but the stored-then-loaded value is not. */

#include "torture.h"

struct flags {
    unsigned a : 3;
    unsigned b : 5;
    unsigned c : 1;
    int s : 4;          /* signed: holds -8..7, sign-extends on read */
};

struct split {
    unsigned x : 20;
    unsigned : 0;       /* zero-width: start a fresh storage unit */
    unsigned y : 12;
};

int
main(void)
{
    struct flags f;
    f.a = 5;            /* 3 bits: 5 */
    f.b = 30;           /* 5 bits: 30 */
    f.c = 1;
    f.s = -3;           /* 4-bit signed */

    t_sint("a", f.a);           /* 5 */
    t_sint("b", f.b);           /* 30 */
    t_sint("c", f.c);           /* 1 */
    t_sint("s", f.s);           /* -3, sign-extended */

    /* overflow on store truncates to the field width */
    f.a = 9;            /* 9 & 7 = 1 */
    t_sint("a_trunc", f.a);     /* 1 */
    f.b = 33;           /* 33 & 31 = 1 */
    t_sint("b_trunc", f.b);     /* 1 */

    /* a signed field storing its max and min */
    f.s = 7;
    t_sint("s_max", f.s);       /* 7 */
    f.s = -8;
    t_sint("s_min", f.s);       /* -8 */

    /* compound assignment through a bit-field */
    f.a = 2;
    f.a += 2;
    t_sint("a_cadd", f.a);      /* 4 */

    struct split sp;
    sp.x = 0xabcde;
    sp.y = 0x123;
    t_hex("x", sp.x);           /* 0xabcde */
    t_hex("y", sp.y);           /* 0x123 */

    return 0;
}
