/* cc_t061_aggregate_init.c : initialized struct/union/array-of-struct globals */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/* Exercises the aggregate byte-image builder: field offsets and padding, an
   8-byte double/long long field (aligned), a char field, an array of structs,
   a nested struct, a pointer field (symbol relocation), a char[] field
   initialized by a string, and brace nesting vs elision. */

struct Point { int x; int y; };
struct Mixed { int a; double d; long long e; char c; };
struct Nested { struct Point p; int z; };
struct HasPtr { int *p; char name[6]; };

int refint = 40;
struct Mixed m = { 1, 2.5, 100, 65 };
struct Point parr[3] = { {1, 2}, {3, 4}, {5, 6} };
struct Nested nst = { {7, 8}, 9 };
struct HasPtr hp = { &refint, "hi" };

int
main(void)
{
    int r = 0;

    /* scalar + 8-byte + char fields, all at their aligned offsets */
    r += (int)m.a + (int)m.d + (int)m.e + (int)m.c;   /* 1+2+100+65 = 168 */

    /* array of structs */
    r += parr[0].x + parr[0].y + parr[1].x + parr[1].y +
         parr[2].x + parr[2].y;                        /* 21 */

    /* nested struct */
    r += nst.p.x + nst.p.y + nst.z;                    /* 24 */

    /* pointer field relocation */
    r += *hp.p;                                        /* 40 */

    /* char[] field initialized by a string literal */
    r += hp.name[0] + hp.name[1];                      /* 'h'+'i' = 209 */

    /* total 462; return a small stable code so the runner can diff it */
    return (r == 462) ? 42 : 1;
}
