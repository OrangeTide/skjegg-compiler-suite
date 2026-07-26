/* half.c : IEEE-754 binary16 <-> binary32 conversion helpers.
 *
 * These are integer-only by design. They never touch an FP or SSE register,
 * so the code generator can call them from the middle of instruction
 * selection (IR_FLH / IR_FSH) without spilling live float temps: on every
 * target the allocatable float registers are preserved because the callee
 * simply never writes them.
 *
 * Calling convention matches the compiler's own: a single 32-bit argument on
 * the stack, result in the integer return register (d0 on ColdFire, eax on
 * x86-32). The value moves as a bit pattern in the low bits of a word.
 *
 *   __skj_extendhfsf(h): low 16 bits of h are a binary16 pattern; returns the
 *                        binary32 bit pattern of the same value (exact).
 *   __skj_truncsfhf(s):  s is a binary32 bit pattern; returns the binary16
 *                        pattern (low 16 bits), rounded to nearest, ties to
 *                        even, overflow to infinity, underflow to zero.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

/* binary16 -> binary32. Widening is always exact, so there is no rounding. */
unsigned int
__skj_extendhfsf(unsigned int hv)
{
    unsigned int h = hv & 0xffffu;
    unsigned int sign = (h & 0x8000u) << 16;    /* half bit 15 -> single bit 31 */
    unsigned int exp = (h >> 10) & 0x1fu;
    unsigned int mant = h & 0x3ffu;
    unsigned int f;

    if (exp == 0x1fu) {
        /* infinity or NaN: single exponent all ones, mantissa carried up */
        f = 0x7f800000u | (mant << 13);
    } else if (exp == 0) {
        if (mant == 0) {
            f = 0;                              /* signed zero */
        } else {
            /* subnormal half: shift the leading 1 up to the hidden bit */
            exp = 127u - 15u + 1u;              /* = 113 */
            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                exp--;
            }
            mant &= 0x3ffu;                     /* drop the now-explicit 1 */
            f = (exp << 23) | (mant << 13);
        }
    } else {
        /* normal: rebias the exponent (15 -> 127), widen the mantissa */
        f = ((exp + (127u - 15u)) << 23) | (mant << 13);
    }
    return sign | f;
}

/* binary32 -> binary16, round to nearest, ties to even. */
unsigned int
__skj_truncsfhf(unsigned int s)
{
    unsigned int sign = (s >> 16) & 0x8000u;    /* single bit 31 -> half bit 15 */
    int exp = (int)((s >> 23) & 0xffu);
    unsigned int mant = s & 0x7fffffu;

    if (exp == 0xff) {
        /* infinity or NaN */
        return sign | (mant ? 0x7e00u : 0x7c00u);
    }

    /* unbiased single exponent, rebiased to half */
    int e = exp - 127 + 15;

    if (e >= 0x1f)
        return sign | 0x7c00u;                  /* overflow to infinity */

    if (e <= 0) {
        /* subnormal half or zero */
        if (e < -10)
            return sign;                        /* too small: signed zero */
        mant |= 0x800000u;                      /* restore the hidden 1 (bit 23) */
        int shift = 14 - e;                     /* drop bits down to the 2^-24 grid */
        unsigned int q = mant >> shift;
        unsigned int rem = mant & ((1u << shift) - 1u);
        unsigned int half = 1u << (shift - 1);
        if (rem > half || (rem == half && (q & 1u)))
            q++;                                /* a carry into the exp field is intended */
        return sign | q;
    }

    /* normal half: keep the top 10 mantissa bits, round on the low 13 */
    unsigned int frac = mant >> 13;
    unsigned int rem = mant & 0x1fffu;
    unsigned int half = 0x1000u;                /* 1 << 12 */
    unsigned int out = ((unsigned int)e << 10) | frac;
    if (rem > half || (rem == half && (frac & 1u)))
        out++;                                  /* carry ripples correctly into exp */
    return sign | out;
}
