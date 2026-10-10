/* Passing a by-value struct whose members are 8-byte-aligned (an embedded
   double or long long) on MIPS o32.  The caller copies the struct word by
   word into the argument structure; an 8-aligned struct starts on an even
   argument word so it matches the callee's 8-aligned home slot, where the
   double/long long field is read with a doubleword load.  Also covers the
   shape that the alignment work exposed: a struct-returning function that
   takes float arguments (the hidden result pointer precedes the floats, so
   each float lands on an odd word unless its home slot is 8-aligned too).
   Value-semantic and self-checking on every target.  Returns 42. */

struct dd { double a; double b; };          /* 16 bytes, 8-aligned */
struct id { int tag; double val; };         /* 8-aligned, a pad word after tag */
struct ll { long long a; long long b; };    /* 16 bytes, 8-aligned */
struct fp { float x; float y; };           /* 8 bytes, 4-aligned */

static double
dd_sum(struct dd s)
{
    return s.a + s.b;
}

/* a scalar before and after the 8-aligned struct (argument-word ordering) */
static double
dd_mix(int lead, struct dd s, int tail)
{
    return (double)lead + s.a + s.b + (double)tail;
}

static long long
ll_sum(struct ll s)
{
    return s.a + s.b;
}

static double
id_sum(struct id s)
{
    return (double)s.tag + s.val;
}

/* struct return with float arguments: the hidden result pointer is the
   leading argument, so x and y follow it on odd words unless 8-aligned */
static struct fp
fp_make(float x, float y)
{
    struct fp r;
    r.x = x;
    r.y = y;
    return r;
}

int
main(void)
{
    struct dd d;
    d.a = 1.5;
    d.b = 2.5;
    if ((int)dd_sum(d) != 4)
        return 1;
    if ((int)dd_mix(10, d, 20) != 34)
        return 2;

    struct ll l;
    l.a = 1000000000000LL;
    l.b = 337LL;
    if (ll_sum(l) != 1000000000337LL)
        return 3;

    struct id i;
    i.tag = 40;
    i.val = 2.0;
    if ((int)id_sum(i) != 42)
        return 4;

    struct fp f = fp_make(1.5f, 2.5f);
    if (f.x != 1.5f || f.y != 2.5f)
        return 5;

    /* pass a freshly returned struct straight through */
    if ((int)dd_sum((struct dd){ 20.0, 22.0 }) != 42)
        return 6;

    return 42;
}
