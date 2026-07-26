/* cc_t072_struct_byval.c : small-struct value semantics and by-value calls.
   The struct copy half works on every target; passing/returning a small
   struct in a register is the x86-64 psABI (P3), so this test is x86-64 only
   (excluded elsewhere, like cc_t071).
   Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

struct P { int x; int y; };            /* 8 bytes, one INTEGER eightbyte */
struct S { char a; short b; int c; };  /* 8 bytes, mixed field widths */
struct TS { long sec; long nsec; };    /* 16 bytes, two INTEGER eightbytes */
struct Cx { double re; double im; };   /* 16 bytes, two SSE eightbytes */
struct Mix { long id; double val; };   /* 16 bytes, INTEGER then SSE */
struct Big { long a, b, c, d; };        /* 32 bytes, MEMORY class */

int
sumP(struct P p)
{
    return p.x + p.y;
}

struct P
makeP(int a, int b)
{
    struct P r;
    r.x = a;
    r.y = b;
    return r;                          /* struct returned in a register */
}

/* a struct argument alongside scalar arguments (register ordering) */
int
mix(int lead, struct P p, int tail)
{
    return lead + p.x + p.y + tail;
}

int
sumS(struct S s)
{
    return s.a + s.b + s.c;
}

/* two INTEGER eightbytes (timespec shape): passed in two gp registers,
   returned in rax:rdx */
long
tssum(struct TS t)
{
    return t.sec + t.nsec;
}

struct TS
tsmake(long s, long n)
{
    struct TS r;
    r.sec = s;
    r.nsec = n;
    return r;
}

/* two SSE eightbytes (complex shape): passed in xmm0:xmm1, returned the same */
double
cxsum(struct Cx c)
{
    return c.re + c.im;
}

/* a mixed struct: an INTEGER eightbyte then an SSE eightbyte */
double
mixsum(struct Mix m)
{
    return (double)m.id + m.val;
}

/* a MEMORY-class struct (>16 bytes): passed as a stack copy, returned through
   a hidden pointer */
long
bigsum(struct Big g)
{
    return g.a + g.b + g.c + g.d;
}

struct Big
bigmake(long a, long b, long c, long d)
{
    struct Big r;
    r.a = a;
    r.b = b;
    r.c = c;
    r.d = d;
    return r;
}

int
main(void)
{
    struct P a;
    a.x = 30;
    a.y = 12;

    /* value-semantic copy: mutating the copy leaves the source intact */
    struct P b = a;
    b.x = 100;
    if (a.x != 30)
        return 1;

    /* struct returned from a call, then passed by value into another */
    struct P p = makeP(30, 12);
    if (sumP(p) != 42)
        return 2;

    /* struct argument between two scalar arguments */
    if (mix(1, p, 2) != 45)
        return 3;

    /* a struct with mixed field widths (char/short/int) */
    struct S s;
    s.a = 5;
    s.b = 7;
    s.c = 30;
    if (sumS(s) != 42)
        return 4;

    /* pass a freshly returned struct straight through */
    if (sumP(makeP(20, 22)) != 42)
        return 5;

    /* two INTEGER eightbytes (rax:rdx return, two gp arg registers) */
    struct TS ts = tsmake(1000, 337);
    if (ts.sec != 1000 || ts.nsec != 337)
        return 6;
    if (tssum(ts) != 1337)
        return 7;

    /* two SSE eightbytes (xmm0:xmm1) */
    struct Cx cx;
    cx.re = 1.5;
    cx.im = 2.5;
    if ((int)cxsum(cx) != 4)
        return 8;

    /* a mixed INTEGER/SSE struct */
    struct Mix mx;
    mx.id = 40;
    mx.val = 2.0;
    if ((int)mixsum(mx) != 42)
        return 9;

    /* a MEMORY-class struct: hidden-pointer return, stack-copy argument */
    struct Big b32 = bigmake(10, 11, 12, 9);
    if (b32.a != 10 || b32.d != 9)
        return 10;
    if (bigsum(b32) != 42)
        return 11;
    if (bigsum(bigmake(1, 2, 3, 36)) != 42)
        return 12;

    return 42;
}
