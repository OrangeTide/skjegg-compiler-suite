/* cc_t063_designated_init.c : designated initializers (.field / [index]) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/* Exercises designated initializers through the aggregate byte-image builder:
   out-of-order struct field designators, a designator followed by positional
   elements, a sparse fixed-size array, a []-sized array whose length comes
   from the highest index, and an array of designated structs. */

struct S { int a; int b; int c; double d; };

struct S s1 = { .a = 1, .c = 3, .b = 2, .d = 4.5 };   /* out of order */
struct S s2 = { .a = 10, 20, 30 };                     /* designated then positional */
int lut[8] = { [2] = 5, [5] = 9, [6] = 1 };            /* sparse fixed array */
int lut2[] = { [3] = 7, [1] = 4 };                     /* []-sized to 4 by max index */
struct S arr[2] = { { .a = 1, .b = 2 }, { .c = 3 } };  /* array of designated structs */

int
main(void)
{
    int r = 0;

    r += s1.a + s1.b + s1.c + (int)s1.d;        /* 1+2+3+4 = 10 */
    r += s2.a + s2.b + s2.c;                     /* 10+20+30 = 60 */
    r += lut[2] + lut[5] + lut[6] + lut[0];      /* 5+9+1+0 = 15 */
    r += lut2[3] + lut2[1];                      /* 7+4 = 11 */
    r += (int)(sizeof(lut2) / sizeof(int));      /* 4 */
    r += arr[0].a + arr[0].b + arr[1].c;         /* 1+2+3 = 6 */

    return (r == 106) ? 42 : (r & 255);
}
