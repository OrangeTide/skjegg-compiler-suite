/* cc_t074_hfa.c : register struct-by-value shared by the two psABI targets.
   x86-64 (SysV eightbytes) and arm64 (AAPCS64 HFA / small aggregate) both pass
   these in registers.  No >16-byte struct, so it needs neither the SysV stack
   copy nor the arm64 x8 path; it runs on x86-64 and arm64, excluded on the
   stack-convention targets (ColdFire, RISC-V).
   Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

struct P { int x; int y; };            /* 8 bytes: one integer register */
struct TS { long a; long b; };         /* 16 bytes: two integer registers */
struct Cx { double re; double im; };   /* a 2-member double HFA / two fp regs */
struct Q { double a, b, c, d; };       /* a 4-member double HFA / four fp regs */
struct V2 { float x; float y; };       /* float HFA (s0,s1) / one SSE eightbyte */
struct V4 { float a, b, c, d; };       /* float HFA (s0..s3) / two SSE eightbytes */

int
psum(struct P p)
{
    return p.x + p.y;
}

long
tssum(struct TS t)
{
    return t.a + t.b;
}

struct TS
tsmk(long a, long b)
{
    struct TS r;
    r.a = a;
    r.b = b;
    return r;
}

double
cxsum(struct Cx c)
{
    return c.re + c.im;
}

struct Cx
cxmk(double a, double b)
{
    struct Cx r;
    r.re = a;
    r.im = b;
    return r;
}

double
qsum(struct Q q)
{
    return q.a + q.b + q.c + q.d;
}

float
v2sum(struct V2 v)
{
    return v.x + v.y;
}

struct V2
v2mk(float a, float b)
{
    struct V2 r;
    r.x = a;
    r.y = b;
    return r;
}

float
v4sum(struct V4 v)
{
    return v.a + v.b + v.c + v.d;
}

int
main(void)
{
    struct P p;
    p.x = 30;
    p.y = 12;
    if (psum(p) != 42)
        return 1;

    /* two integer registers, and a struct returned in two registers */
    struct TS t = tsmk(1000, 337);
    if (t.a != 1000 || t.b != 337)
        return 2;
    if (tssum(t) != 1337)
        return 3;

    /* a 2-member double HFA, passed and returned in fp registers */
    struct Cx c = cxmk(1.5, 2.5);
    if (c.re != 1.5 || c.im != 2.5)
        return 4;
    if ((int)cxsum(c) != 4)
        return 5;

    /* a 4-member double HFA */
    struct Q q;
    q.a = 1.0;
    q.b = 2.0;
    q.c = 3.0;
    q.d = 4.0;
    if ((int)qsum(q) != 10)
        return 6;

    /* a 2-member float HFA, passed and returned in single-precision fp regs */
    struct V2 v = v2mk(1.5f, 2.5f);
    if (v.x != 1.5f || v.y != 2.5f)
        return 7;
    if ((int)v2sum(v) != 4)
        return 8;

    /* a 4-member float HFA */
    struct V4 w;
    w.a = 1.0f;
    w.b = 2.0f;
    w.c = 3.0f;
    w.d = 4.0f;
    if ((int)v4sum(w) != 10)
        return 9;

    return 42;
}
