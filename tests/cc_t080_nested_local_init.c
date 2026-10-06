/* Local aggregate initializers with nested braces (C 6.7.8): nested structs
   and arrays, array-of-struct, multidimensional arrays with and without brace
   elision, partial initializers (the rest zero), array and field designators,
   runtime element values, a string in a struct field, and a union. Each check
   returns its own number on failure; a clean run returns 42. Integer only, so
   every backend and JIT runs it. */

struct inner { int a, b; };
struct outer { int x; struct inner n; };
struct p { int x, y; };
struct w { char name[8]; int n; };
union u { int i; char c[4]; };

int
main(void)
{
    /* nested struct */
    {
        struct outer o = { 1, { 2, 3 } };
        if (o.x != 1 || o.n.a != 2 || o.n.b != 3)
            return 1;
    }

    /* array of struct */
    {
        struct p ps[2] = { { 1, 2 }, { 3, 4 } };
        if (ps[0].x != 1 || ps[0].y != 2 || ps[1].x != 3 || ps[1].y != 4)
            return 2;
    }

    /* 2D array, fully braced */
    {
        int a[2][2] = { { 1, 2 }, { 3, 4 } };
        if (a[0][0] != 1 || a[0][1] != 2 || a[1][0] != 3 || a[1][1] != 4)
            return 3;
    }

    /* 2D array, brace elision */
    {
        int a[2][2] = { 1, 2, 3, 4 };
        if (a[0][0] != 1 || a[0][1] != 2 || a[1][0] != 3 || a[1][1] != 4)
            return 4;
    }

    /* partial initializer: the rest is zero */
    {
        int a[5] = { 7, 8 };
        if (a[0] != 7 || a[1] != 8 || a[2] != 0 || a[3] != 0 || a[4] != 0)
            return 5;
        struct outer o = { 9 };
        if (o.x != 9 || o.n.a != 0 || o.n.b != 0)
            return 6;
    }

    /* array designators (length is one past the highest index) */
    {
        int a[5] = { [4] = 9, [1] = 2 };
        if (a[0] != 0 || a[1] != 2 || a[2] != 0 || a[3] != 0 || a[4] != 9)
            return 7;
    }

    /* field designators, out of order */
    {
        struct { int x, y, z; } s = { .z = 3, .x = 1 };
        if (s.x != 1 || s.y != 0 || s.z != 3)
            return 8;
    }

    /* runtime element values in a nested initializer */
    {
        int k = 7;
        int a[3][2] = { { 1, k }, { k + 1, 4 } };
        if (a[0][1] != 7 || a[1][0] != 8 || a[2][0] != 0 || a[2][1] != 0)
            return 9;
        struct p q = { k, k * 2 };
        if (q.x != 7 || q.y != 14)
            return 10;
    }

    /* a string literal initializing a char-array field */
    {
        struct w a = { "bob", 5 };
        if (a.name[0] != 'b' || a.name[1] != 'o' || a.name[2] != 'b' ||
            a.name[3] != 0 || a.n != 5)
            return 11;
    }

    /* a union takes its first member */
    {
        union u x = { 0x12345678 };
        if (x.i != 0x12345678)
            return 12;
    }

    return 42;
}
