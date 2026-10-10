/* A member access, index, or address-of applied directly to the result of a
   function returning a struct by value (mk().a), without binding the result to
   a variable first.  This reported "not an lvalue" before: the call result is
   materialized in a hidden slot, so it does have an address.  Self-checking,
   returns 42.

   Runs on every target: struct return by value is supported everywhere (the
   register-ABI targets return small structs in registers, ColdFire and MIPS
   through a hidden caller-allocated result pointer). */

struct point {
    int x;
    int y;
};

struct line {
    struct point a;     /* a nested struct field */
    int len;
};

struct box {
    int v[4];           /* an array field */
    int tag;
};

struct big {
    int a, b, c, d, e;  /* > 16 bytes: the memory-return path */
};

static struct point
mkpoint(int x, int y)
{
    struct point p = { x, y };
    return p;
}

static struct line
mkline(void)
{
    struct line l = { { 30, 4 }, 8 };
    return l;
}

static struct box
mkbox(void)
{
    struct box b = { { 1, 2, 3, 4 }, 99 };
    return b;
}

static struct big
mkbig(void)
{
    struct big g = { 10, 11, 12, 1, 8 };
    return g;
}

static int
take(int v)
{
    return v;
}

int
main(void)
{
    /* a scalar member directly on the call result */
    if (mkpoint(40, 2).x + mkpoint(0, 2).y != 42)
        return 1;
    /* a nested member chain mk().a.b */
    if (mkline().a.x + mkline().a.y != 34)
        return 2;
    if (mkline().len != 8)
        return 3;
    /* an index of an array member mk().v[i] */
    if (mkbox().v[0] + mkbox().v[3] + mkbox().tag != 104)
        return 4;
    /* a member of a large (memory-returned) struct */
    if (mkbig().a + mkbig().b + mkbig().c != 33)
        return 5;
    if (mkbig().d + mkbig().e != 9)
        return 6;
    /* the call-result member passed as an argument */
    if (take(mkpoint(20, 22).x + mkpoint(20, 22).y) != 42)
        return 7;
    /* address-of a call-result member, read back immediately */
    {
        int *px = &mkpoint(15, 27).x;
        if (*px != 15)
            return 8;
    }
    /* a member directly off an indirect (function-pointer) call result */
    {
        struct point (*fp)(int, int) = mkpoint;
        if (fp(19, 23).x + fp(19, 23).y != 42)
            return 9;
    }
    return 42;
}
