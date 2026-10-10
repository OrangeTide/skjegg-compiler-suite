/* Compound assignment across every operator and several storage widths, on a
   local, through a pointer, into a struct member, and into an array element,
   for int, unsigned, and long long. The read-modify-write must compute and
   store the same value on every backend. */

#include "torture.h"

struct box { int v; };

int
main(void)
{
    /* every operator on an int lvalue */
    {
        int x = 100;
        x += 5;   t_sint("add", x);   /* 105 */
        x -= 13;  t_sint("sub", x);   /* 92  */
        x *= 3;   t_sint("mul", x);   /* 276 */
        x /= 4;   t_sint("div", x);   /* 69  */
        x %= 20;  t_sint("mod", x);   /* 9   */
        x <<= 4;  t_sint("shl", x);   /* 144 */
        x >>= 2;  t_sint("shr", x);   /* 36  */
        x &= 0x1e; t_sint("and", x);  /* 36 & 30 = 36 */
        x |= 1;   t_sint("or",  x);   /* 37  */
        x ^= 0xf; t_sint("xor", x);   /* 37 ^ 15 = 42 */
    }

    /* unsigned compound ops must stay unsigned: >>= fills with zero, and /=
       and %= treat the value as unsigned (its signed reading is negative) */
    {
        unsigned u = 0x80000000u;
        u >>= 4;
        t_hex("ushr", (unsigned long long)u);   /* 0x08000000, not 0xf8000000 */
    }
    {
        unsigned a = 0x80000000u;   /* 2147483648 */
        a /= 7u;
        t_hex("udiv", (unsigned long long)a);   /* 306783378 = 0x1249_2492 */
        unsigned b = 0x80000000u;
        b %= 7u;
        t_hex("umod", (unsigned long long)b);   /* 2 */
    }

    /* long long compound assignment keeps the full width */
    {
        long long w = 0x100000000LL;
        w += 0x2a;
        t_hex("ll_add", (unsigned long long)w);   /* 0x10000002a */
        w <<= 1;
        t_hex("ll_shl", (unsigned long long)w);   /* 0x200000054 */
    }

    /* through a pointer */
    {
        int a = 10;
        int *p = &a;
        *p += 32;
        t_sint("ptr", a);   /* 42 */
    }

    /* into a struct member and an array element */
    {
        struct box b = { 7 };
        b.v *= 6;
        t_sint("member", b.v);   /* 42 */

        int arr[4] = { 1, 2, 3, 4 };
        arr[2] += arr[1];
        arr[2] *= 2;
        t_sint("elem", arr[2]);  /* (3+2)*2 = 10 */
    }

    return 0;
}
