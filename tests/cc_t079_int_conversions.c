/* Integer promotions and the usual arithmetic conversions (C 6.3.1.1,
   6.3.1.8). Each check fails under the old "either operand unsigned" rule and
   holds under the common-type rule. The result is carried in the exit code:
   the first failing check returns its number, a clean run returns 42. No
   division, so no 64-bit divide helper is needed. */

int
main(void)
{
    /* long long with unsigned int: common type is signed long long (it
       represents all unsigned int values), so the compare is signed. */
    {
        long long big = -1;
        unsigned u = 1;
        if (!(big < u))
            return 1;
    }

    /* a shift's type is the promoted left operand; a signed left shifts
       arithmetically even with an unsigned count. */
    {
        int x = -16;
        unsigned n = 2;
        if ((x >> n) != -4)
            return 2;
        long long y = -256;
        unsigned m = 3;
        if ((y >> m) != -32)
            return 3;
    }

    /* a sub-int unsigned type promotes to signed int, so this compare is
       signed. */
    {
        unsigned char c = 1;
        int i = -1;
        if (!(c > i))
            return 4;
    }

    /* signed minus unsigned (equal rank) is unsigned int; widening to long
       long zero-extends, so the value is a large positive, not -1. */
    {
        int a = 0;
        unsigned b = 1;
        long long r = a - b;
        if (r != 0xFFFFFFFFLL)
            return 5;
    }

    /* the usual conversions set the result width, visible through sizeof. */
    if (sizeof((char)1 + (char)1) != 4)
        return 6;
    if (sizeof((long long)1 + 1) != 8)
        return 7;

    return 42;
}
