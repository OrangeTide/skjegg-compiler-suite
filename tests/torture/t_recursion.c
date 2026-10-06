/* Deep recursion and large stack frames: every backend's prologue, epilogue,
   and frame layout must agree on the computed result. Ackermann exercises deep
   nested calls; the large-frame function forces a big stack allocation and
   touches both ends of it. */

#include "torture.h"

static int
ack(int m, int n)
{
    if (m == 0)
        return n + 1;
    if (n == 0)
        return ack(m - 1, 1);
    return ack(m - 1, ack(m, n - 1));
}

static long long
sumrec(int n)
{
    if (n == 0)
        return 0;
    return (long long)n + sumrec(n - 1);
}

/* a large frame: a local array bigger than any argument area, written at both
   ends and summed, so an off-by-frame-size error shows */
static long long
bigframe(int seed)
{
    int a[512];
    long long s = 0;
    int i;
    for (i = 0; i < 512; i++)
        a[i] = seed + i * 3;
    for (i = 0; i < 512; i++)
        s += a[i] ^ (a[511 - i] << 1);
    return s;
}

int
main(void)
{
    /* sumrec's depth stays well under the smallest peer's fixed guest stack
       (the in-tree x86-64 runtime reserves ~384 KB); frame save/restore is
       exercised long before any depth limit. */
    t_sint("ack_3_6", ack(3, 6));
    t_sint("sumrec_500", sumrec(500));
    t_hex("bigframe", (unsigned long long)bigframe(7));
    return 0;
}
