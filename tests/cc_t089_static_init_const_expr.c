/* A scalar static or global initializer that is a constant expression (not a
   bare literal) must be folded and emitted, not silently dropped to zero.
   Before the fix, only a plain integer literal and a negated literal were
   handled; any other constant expression (A * B, 1 << N, a parenthesized mix)
   produced a zero-initialized .space slot, so the variable started at 0.
   Self-checking, returns 42. */

static int g_mul = 2 * 3;                   /* 6 */
int g_shift = (1 << 4) | 2;                 /* 18, file-scope non-static */
static long g_long = 100L * 100;            /* 10000 */
static unsigned g_u = 0xffu & (7 << 1);     /* 14 */
static int g_neg = -(3 * 4);                /* -12 */
static int g_tern = (1 < 2) ? 7 * 3 : 0;    /* 21 */

int
main(void)
{
    static int s_mul = 5 * 5;               /* 25, function-local static */

    if (g_mul != 6)
        return 1;
    if (g_shift != 18)
        return 2;
    if (g_long != 10000L)
        return 3;
    if (g_u != 14u)
        return 4;
    if (g_neg != -12)
        return 5;
    if (g_tern != 21)
        return 6;
    if (s_mul != 25)
        return 7;
    return 42;
}
