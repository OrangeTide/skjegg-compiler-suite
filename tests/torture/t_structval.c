/* Passing and returning structs by value, the most ABI-sensitive path (each
   target has its own rule for which struct rides in registers and which goes
   through memory). The structs use only int/char/short/long long, so their
   layout is identical on every target (no long or pointer field), and each
   function only reads back what it was given, so a correct ABI makes every
   peer agree with the host gcc oracle. */

#include "torture.h"

struct small { int a, b; };
struct mixed { char c; short s; int i; };
struct big   { int a, b, c, d, e; };
struct wide  { long long x; int y; };

static struct small
mk_small(int a, int b)
{
    struct small r;
    r.a = a;
    r.b = b;
    return r;
}

static int
sum_small(struct small s)
{
    return s.a * 3 + s.b;
}

static int
sum_mixed(struct mixed m)
{
    return m.c + m.s * 2 + m.i * 4;
}

static int
sum_big(struct big b)
{
    return b.a + b.b + b.c + b.d + b.e;
}

static long long
sum_wide(struct wide w)
{
    return w.x + w.y;
}

int
main(void)
{
    struct small s = mk_small(5, 7);
    t_sint("small", sum_small(s));          /* 5*3+7 = 22 */
    t_sint("small_direct", sum_small(mk_small(10, 1)));  /* 31 */

    struct mixed m = { 1, 2, 3 };
    t_sint("mixed", sum_mixed(m));          /* 1 + 4 + 12 = 17 */

    struct big b = { 10, 20, 30, 40, 50 };
    t_sint("big", sum_big(b));              /* 150 */

    struct wide w;
    w.x = 0x100000000LL;
    w.y = 42;
    t_hex("wide", (unsigned long long)sum_wide(w));  /* 0x10000002a */

    /* a returned struct whose fields are read directly off the call result */
    t_sint("ret_a", mk_small(100, 200).a);  /* 100 */
    t_sint("ret_b", mk_small(100, 200).b);  /* 200 */

    /* struct copy by assignment, then read the copy */
    {
        struct big b2 = b;
        b2.c = 300;
        t_sint("copy", sum_big(b2));        /* 10+20+300+40+50 = 420 */
        t_sint("orig", sum_big(b));         /* unchanged: 150 */
    }

    return 0;
}
