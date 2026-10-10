/* The C11 _Bool scalar type.  A _Bool holds only 0 or 1: converting any scalar
   to it yields 1 if the value compares unequal to zero, else 0 (6.3.1.2).  This
   exercises every conversion-to-_Bool path: a static global, a static aggregate
   field, an automatic initializer, a plain assignment, a cast, a function
   return, a function argument, a compound assignment, an array element, and an
   i64 source whose low 32 bits are zero (which must not truncate to 0).  <stdbool.h>
   bool / true / false are the same type.  Returns 42. */

#include <stdbool.h>

static _Bool sg = 5;                   /* -> 1 */
static _Bool sz = 0;                   /* -> 0 */
static _Bool sf = 0.5;                 /* a nonzero float constant -> 1 */

struct S { _Bool a; _Bool b; int c; };
static struct S ss = { 7, 0, 99 };     /* a -> 1, b -> 0 */

static _Bool
ret5(void)
{
    return 5;                          /* normalized to 1 */
}

static int
takes(_Bool x)
{
    return x;                          /* receives 0 or 1 */
}

int
main(void)
{
    if (sg != 1 || sz != 0 || sf != 1) return 1;
    if (ss.a != 1 || ss.b != 0 || ss.c != 99) return 2;

    _Bool b = 42;                      /* automatic init -> 1 */
    if (b != 1) return 3;
    b = 0;
    if (b != 0) return 4;
    b = 7;                             /* assignment -> 1 */
    if (b != 1) return 5;
    b = 0.0;                           /* float assignment -> 0 */
    if (b != 0) return 6;

    int x = (_Bool)256;                /* cast: 256 is nonzero -> 1 */
    if (x != 1) return 7;
    if ((_Bool)0 != 0) return 8;

    if (ret5() != 1) return 9;
    if (ret5() + 1 != 2) return 10;    /* the return value is 1, used directly */
    if (takes(99) != 1) return 11;     /* the argument is normalized */

    long long big = 0x100000000LL;     /* nonzero, but the low 32 bits are 0 */
    _Bool fromll = big;                /* must be 1, not a truncation to 0 */
    if (fromll != 1) return 12;

    bool t = true, f = false;
    if (!t || f) return 13;

    _Bool arr[2] = { 3, 0 };           /* -> 1, 0 */
    if (arr[0] != 1 || arr[1] != 0) return 14;

    b = 0;
    b += 5;                            /* compound assign: 0 + 5 -> 1 */
    if (b != 1) return 15;

    if (sizeof(_Bool) != 1) return 16;

    return 42;
}
