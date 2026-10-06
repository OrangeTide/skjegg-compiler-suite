/* Memory, pointers, and aggregate layout: a global array walked by pointer
   arithmetic, and a struct of mixed-width fields read and written through a
   pointer. The printed values must agree even though each target lays the
   struct out for its own ABI, since the program only ever reads back what it
   wrote. */

#include "torture.h"

struct rec {
    int a;
    signed char b;          /* pinned signedness: plain char is target-defined */
    long long c;
    short d;
    unsigned e;
};

static int g[16];
static struct rec recs[4];

static long long
walk(const int *p, int n)
{
    long long s = 0;
    const int *end = p + n;
    while (p < end) {
        s = s * 31 + *p;
        p++;
    }
    return s;
}

int
main(void)
{
    int i;
    long long acc = 0;

    for (i = 0; i < 16; i++)
        g[i] = (i * 2654435761u) & 0xffff;
    t_hex("walk", (unsigned long long)walk(g, 16));

    for (i = 0; i < 4; i++) {
        struct rec *r = &recs[i];
        r->a = i * 7 - 3;
        r->b = (char)(i * 40 + 1);
        r->c = (long long)i << 40 | 0x5555;
        r->d = (short)(-i * 300);
        r->e = 0x80000000u + (unsigned)i;
    }
    for (i = 0; i < 4; i++) {
        struct rec *r = &recs[i];
        acc += r->a;
        acc += (long long)r->b;          /* sign-extends: signed char */
        acc ^= r->c;
        acc += (long long)r->d;          /* sign-extends */
        acc += (long long)r->e;          /* zero-extends: unsigned */
    }
    t_hex("recs", (unsigned long long)acc);
    return 0;
}
