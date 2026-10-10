/* A float or double static/global initializer that is a constant expression,
   not a bare literal, must be folded and encoded.  Before the float const
   folder, `static double d = 1.5 + 2.0;` reached encode_scalar as a BINOP and
   was rejected (or silently zero on the earlier scalar path).  The folder must
   respect C typing: `1 / 2` is integer division (0) even in a double slot,
   while `1.0 / 2` is 0.5.  Self-checking, returns 42. */

static double d  = 1.5 + 2.0;        /* 3.5 */
static double m  = 3.0 * 2.0;        /* 6.0 */
static float  f  = 2.5f - 0.5f;      /* 2.0f */
double        pub = 10.0 / 4.0;      /* 2.5, file-scope non-static */
static double mix = 2 + 0.5;         /* int + double -> 2.5 */
static double idiv = 1 / 2;          /* integer division -> 0.0 */
static double fdiv = 1.0 / 2;        /* float division -> 0.5 */
static double neg  = -(1.5 + 0.5);   /* -2.0 */

int
main(void)
{
    static double ls = 0.25 * 3.0;   /* function-local static, 0.75 */

    if (d != 3.5)
        return 1;
    if (m != 6.0)
        return 2;
    if (f != 2.0f)
        return 3;
    if (pub != 2.5)
        return 4;
    if (mix != 2.5)
        return 5;
    if (idiv != 0.0)
        return 6;
    if (fdiv != 0.5)
        return 7;
    if (neg != -2.0)
        return 8;
    if (ls != 0.75)
        return 9;
    return 42;
}
