/* Pointers and arrays: indexing, pointer arithmetic, the array/pointer duality,
   pointer subtraction (printed as an element count, never an address, so the
   result is width-independent), and pointer comparisons. Addresses themselves
   differ per target and are never emitted; only the derived integers are. */

#include "torture.h"

static int g[8] = { 0, 10, 20, 30, 40, 50, 60, 70 };

static int
sum_range(const int *lo, const int *hi)
{
    int s = 0;
    while (lo < hi)
        s += *lo++;
    return s;
}

int
main(void)
{
    /* a[i], *(a+i), i[a] all name the same element */
    t_sint("idx", g[3] + *(g + 5) + 6[g]);   /* 30 + 50 + 60 = 140 */

    /* walk with a moving pointer */
    {
        int *p = g;
        int acc = 0;
        for (int i = 0; i < 8; i++)
            acc += *p++;
        t_sint("walk", acc);   /* 0+10+..+70 = 280 */
    }

    /* pointer subtraction yields the element count between two pointers */
    {
        int *a = &g[1];
        int *b = &g[7];
        t_sint("pdiff", (int)(b - a));   /* 6 */
    }

    /* the range sum via a pointer pair */
    t_sint("range", sum_range(&g[2], &g[6]));   /* 20+30+40+50 = 140 */

    /* pointer comparisons */
    {
        int *a = &g[0];
        int *b = &g[4];
        t_sint("lt", a < b);        /* 1 */
        t_sint("ge", a >= b);       /* 0 */
        t_sint("eq", a == &g[0]);   /* 1 */
    }

    /* a two-dimensional access through row pointers */
    {
        int m[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
        int acc = 0;
        for (int r = 0; r < 2; r++)
            for (int c = 0; c < 3; c++)
                acc += m[r][c] * (r + 1);
        /* row0: 1+2+3=6; row1: (4+5+6)*2=30; total 36 */
        t_sint("mat", acc);
    }

    return 0;
}
