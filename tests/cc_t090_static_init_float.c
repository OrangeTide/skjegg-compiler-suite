/* A scalar float or double static/global initialized by a literal must be
   encoded, not dropped to zero.  Before the fix, the float/double scalar
   initializer was never captured (only int literals, strings, addresses, and
   initializer lists were), so `static double d = 1.5;` silently became 0.0.
   An int literal in a float slot must convert (2 -> 2.0), and a negated float
   literal must carry its sign.  Self-checking, returns 42. */

static double d = 1.5;
static float  f = 2.5f;
double        pub = 3.25;        /* file-scope, non-static */
static double di = 2;            /* int literal into a double slot */
static double nd = -1.5;         /* negated double literal */
static float  nf = -2.5f;        /* negated float literal */
static int    ni = -5;           /* negated int literal (unchanged path) */

int
main(void)
{
    static double ld = 0.75;     /* function-local static */

    if (d != 1.5)
        return 1;
    if (f != 2.5f)
        return 2;
    if (pub != 3.25)
        return 3;
    if (di != 2.0)
        return 4;
    if (nd != -1.5)
        return 5;
    if (nf != -2.5f)
        return 6;
    if (ni != -5)
        return 7;
    if (ld != 0.75)
        return 8;
    return 42;
}
