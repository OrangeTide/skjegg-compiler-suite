/* Globals and static storage: an initialized .data array, a static local that
   persists across calls, a global struct array, and a nested initializer.
   These exercise the data-section emission and the static-local init path, both
   of which a backend lays out for itself; every peer must read back the same
   values. No long or pointer field appears, so the layout is identical on every
   target. */

#include "torture.h"

static int table[6] = { 1, 2, 3, 5, 8, 13 };

struct point { int x, y; };
static struct point pts[3] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };

struct nest { int a; struct point p; int b; };
static struct nest ns = { 10, { 20, 30 }, 40 };

/* a static local keeps its value between calls and is initialized once */
static int
counter(void)
{
    static int n = 100;
    n += 7;
    return n;
}

/* a partially-initialized global: the rest is zero */
static int sparse[8] = { [2] = 22, [5] = 55 };

int
main(void)
{
    int acc = 0;
    for (int i = 0; i < 6; i++)
        acc += table[i] * (i + 1);
    t_sint("table", acc);   /* 1+4+9+20+40+78 = 152 */

    int ps = 0;
    for (int i = 0; i < 3; i++)
        ps += pts[i].x * 10 + pts[i].y;
    t_sint("pts", ps);      /* 12+34+56 = 102 */

    t_sint("nest", ns.a + ns.p.x + ns.p.y + ns.b);  /* 10+20+30+40 = 100 */

    t_sint("static1", counter());   /* 107 */
    t_sint("static2", counter());   /* 114 */
    t_sint("static3", counter());   /* 121 */

    int sp = 0;
    for (int i = 0; i < 8; i++)
        sp += sparse[i];
    t_sint("sparse", sp);   /* 22 + 55 = 77 */

    /* write back into a global and read it */
    table[0] = 99;
    t_sint("wb", table[0] + table[5]);  /* 99 + 13 = 112 */

    return 0;
}
