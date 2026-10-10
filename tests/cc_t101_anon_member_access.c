/* C11 anonymous struct/union members expose their inner fields to the
   enclosing struct (6.7.2.1p13): `o.a` reaches a field `a` declared in an
   unnamed member.  This exercises every path that resolves a member name:
   value read, write through a pointer, address-of, offsetof constant fold, a
   bit-field inside an anonymous member, nested anonymous members, and a
   designated initializer (static and automatic) naming an inner field.
   Returns 42. */

#include <stddef.h>

struct outer {
    struct { int ia; int ib; };     /* anonymous struct */
    int tag;
    union { int u0; int u1; };       /* anonymous union */
};

struct nested {
    struct {
        union { int x; };            /* anonymous union inside anonymous struct */
        int y;
    };
    int z;
};

struct bits {
    struct { unsigned b0 : 3; unsigned b1 : 5; };   /* bit-fields in an anon member */
    int after;
};

/* static global, designated init naming inner fields out of order */
static struct outer g = { .ib = 5, .tag = 7, .u1 = 10, .ia = 1 };

int
main(void)
{
    struct outer o;
    o.ia = 1; o.ib = 2; o.tag = 3; o.u0 = 4;
    if (o.ia + o.ib + o.tag + o.u0 != 10) return 1;

    /* address-of through an anonymous member */
    int *p = &o.ib;
    *p = 20;
    if (o.ib != 20) return 2;

    /* offsetof folds through the anonymous members */
    if (offsetof(struct outer, ia) != 0) return 3;
    if (offsetof(struct outer, ib) != 4) return 4;
    if (offsetof(struct outer, u0) != 12) return 5;

    /* nested anonymous members */
    struct nested nn;
    nn.x = 7; nn.y = 8; nn.z = 9;
    if (nn.x + nn.y + nn.z != 24) return 6;
    if (offsetof(struct nested, x) != 0) return 7;
    if (offsetof(struct nested, y) != 4) return 8;
    if (offsetof(struct nested, z) != 8) return 9;

    /* bit-fields inside an anonymous member, including compound assign */
    struct bits bb;
    bb.b0 = 5; bb.b1 = 17; bb.after = 100;
    if (bb.b0 != 5 || bb.b1 != 17 || bb.after != 100) return 10;
    bb.b0 += 1;
    if (bb.b0 != 6) return 11;

    /* automatic designated init naming inner fields */
    struct outer a = { .ib = 5, .tag = 7, .u0 = 20 };
    if (a.ia != 0 || a.ib != 5 || a.tag != 7 || a.u0 != 20) return 12;

    /* static designated init naming inner fields */
    if (g.ia != 1 || g.ib != 5 || g.tag != 7 || g.u1 != 10) return 13;

    return 42;
}
