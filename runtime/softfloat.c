/* softfloat.c : integer-only IEEE-754 binary64 arithmetic for a target with
 * no floating-point unit (the PlayStation R3051, an R3000A with no
 * coprocessor 1).
 *
 * The MIPS backend's soft-float mode keeps every double as a 64-bit integer
 * bit pattern and lowers each float operation to a call into this file. The
 * code here never touches a floating-point register: it decodes the bit
 * pattern, does the arithmetic with integer operations, and re-encodes the
 * result. It is therefore correct on a machine with no FPU, and it produces
 * the same bit pattern an IEEE-754 double unit would, so a build that uses it
 * runs identically under qemu-mipsel (which does model an FPU) and on real
 * PS1 hardware (which does not).
 *
 * The single-precision path is by widening: a binary32 operation extends both
 * operands to binary64, runs the double core, and rounds the result back to
 * binary32. This matches the toolkit's RISC-V single-precision decision
 * (compute in the wider type, then round) and needs only the two conversion
 * helpers below plus the binary64 core.
 *
 * Calling convention: these are ordinary C functions taking and returning
 * integer types, so a gcc-built o32 caller passes a 64-bit pattern in a
 * register pair and gets the result back in $v0/$v0:$v1. The backend calls
 * them the same way it calls the half-float helpers. Rounding is
 * round-to-nearest, ties to even. The helpers do not read or set any host
 * rounding mode, so they stay WebAssembly-clean like the rest of the toolkit.
 *
 * The 64-bit integer multiply, divide and shift these functions lean on are
 * the target toolchain's own integer helpers (libgcc's __udivdi3 and the
 * like), which have nothing to do with the FPU and are always present.
 */

typedef unsigned int u32;
typedef unsigned long long u64;
typedef int s32;

#define SIGBITS   52
#define EXPMAX    0x7ff
#define BIAS      1023
#define IMPLICIT  (1ULL << SIGBITS)
#define SIGMASK   (IMPLICIT - 1)
#define SIGNBIT   (1ULL << 63)
#define ABSMASK   0x7fffffffffffffffULL
#define INFBITS   0x7ff0000000000000ULL
#define QNANBITS  0x7ff8000000000000ULL

/* count leading zeros of a nonzero 64-bit value */
static int
clz64(u64 x)
{
    int n = 0;
    if (!(x & 0xffffffff00000000ULL)) { n += 32; x <<= 32; }
    if (!(x & 0xffff000000000000ULL)) { n += 16; x <<= 16; }
    if (!(x & 0xff00000000000000ULL)) { n += 8;  x <<= 8;  }
    if (!(x & 0xf000000000000000ULL)) { n += 4;  x <<= 4;  }
    if (!(x & 0xc000000000000000ULL)) { n += 2;  x <<= 2;  }
    if (!(x & 0x8000000000000000ULL)) { n += 1; }
    return n;
}

/* 64x64 -> 128 unsigned multiply, returned as (*hi, *lo). */
static void
umul64(u64 x, u64 y, u64 *hi, u64 *lo)
{
    u64 x0 = x & 0xffffffffULL, x1 = x >> 32;
    u64 y0 = y & 0xffffffffULL, y1 = y >> 32;
    u64 p00 = x0 * y0, p01 = x0 * y1, p10 = x1 * y0, p11 = x1 * y1;
    u64 mid = (p00 >> 32) + (p01 & 0xffffffffULL) + (p10 & 0xffffffffULL);

    *lo = (p00 & 0xffffffffULL) | (mid << 32);
    *hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
}

/* Round a result and pack it.  sign is 0/1.  exp is the unbiased-plus-bias
 * exponent for a significand whose implicit bit sits at bit SIGBITS (52) and
 * which carries three guard bits below bit 0, i.e. sig holds the 53-bit
 * significand shifted left by 3.  The leading 1 is assumed to be at bit
 * SIGBITS+3 = 55 on entry (callers normalize to that).  Handles overflow to
 * infinity and underflow to a subnormal or zero. */
static u64
round_pack(u32 sign, int exp, u64 sig)
{
    u64 s = (u64)sign << 63;
    u64 round, r;

    /* If the value is subnormal or would underflow, shift right so exp == 0,
       accumulating a sticky bit, then round in that position. */
    if (exp <= 0) {
        int shift = 1 - exp;
        u64 sticky;
        if (shift > 63)
            shift = 63;
        sticky = (sig & (((u64)1 << shift) - 1)) != 0;
        sig = (sig >> shift) | sticky;
        exp = 0;
    }

    round = sig & 7;
    r = sig >> 3;                       /* the 53-bit significand */
    if (round > 4 || (round == 4 && (r & 1)))
        r++;                            /* nearest, ties to even */

    if (r & (IMPLICIT << 1)) {          /* round-up carried past bit 52 */
        r >>= 1;
        exp++;
    }

    if (exp >= EXPMAX)
        return s | INFBITS;             /* overflow to infinity */

    /* The result is normal exactly when the significand carries the implicit
       bit; otherwise it is subnormal and the exponent field is zero.  A
       subnormal that rounded up to the implicit bit is the smallest normal. */
    if (r & IMPLICIT) {
        if (exp <= 0)
            exp = 1;
        return s | ((u64)exp << SIGBITS) | (r & SIGMASK);
    }
    return s | (r & SIGMASK);
}

/* decode a as sign/exp/sig (sig 53-bit with implicit bit for normals; for
 * subnormals exp is returned as the effective 1 and the leading bit is not
 * set).  Returns the raw exponent field for special-case detection via *rawexp. */
static void
unpack(u64 a, u32 *sign, int *exp, u64 *sig, int *rawexp)
{
    int e = (a >> SIGBITS) & EXPMAX;
    u64 frac = a & SIGMASK;

    *sign = (u32)(a >> 63) & 1;
    *rawexp = e;
    if (e == 0) {
        *exp = 1;
        *sig = frac;                    /* subnormal: no implicit bit */
    } else {
        *exp = e;
        *sig = frac | IMPLICIT;
    }
}

static u64
add_magnitudes(u32 sign, int aExp, u64 aSig, int bExp, u64 bSig)
{
    /* aExp >= bExp guaranteed by the caller.  Work with 3 guard bits. */
    int shift = aExp - bExp;
    u64 sticky, sum;

    aSig <<= 3;
    bSig <<= 3;
    if (shift) {
        if (shift > 60) {
            sticky = bSig != 0;
            bSig = sticky;
        } else {
            sticky = (bSig & (((u64)1 << shift) - 1)) != 0;
            bSig = (bSig >> shift) | sticky;
        }
    }
    sum = aSig + bSig;
    if (sum & ((u64)1 << (SIGBITS + 4))) {   /* carried into bit 56 */
        sticky = sum & 1;
        sum = (sum >> 1) | sticky;
        aExp++;
    }
    return round_pack(sign, aExp, sum);
}

static u64
sub_magnitudes(u32 sign, int aExp, u64 aSig, int bExp, u64 bSig)
{
    /* aExp >= bExp; when exponents are equal the caller has ordered so that
       aSig >= bSig.  Result may need left-normalization. */
    int shift = aExp - bExp;
    u64 sticky = 0, diff;
    int lz;

    aSig <<= 3;
    bSig <<= 3;
    if (shift) {
        if (shift > 60) {
            sticky = bSig != 0;
            bSig = sticky;
        } else {
            sticky = (bSig & (((u64)1 << shift) - 1)) != 0;
            bSig = (bSig >> shift) | sticky;
        }
    }
    (void)sticky;
    diff = aSig - bSig;                 /* discarded bits jammed into bit 0 above */

    if (diff == 0)
        return 0;                       /* exact cancellation -> +0 */

    /* normalize so the leading 1 returns to bit SIGBITS+3 (55) */
    lz = clz64(diff) - (63 - (SIGBITS + 3));
    if (lz > 0) {
        diff <<= lz;
        aExp -= lz;
    } else if (lz < 0) {
        u64 lost = diff & (((u64)1 << (-lz)) - 1);
        diff = (diff >> (-lz)) | (lost != 0);
        aExp -= lz;
    }
    return round_pack(sign, aExp, diff);
}

/* the core add; __skj_dsub flips b's sign into this */
u64
__skj_dadd(u64 a, u64 b)
{
    u32 aSign, bSign;
    int aExp, bExp, aRaw, bRaw;
    u64 aSig, bSig;
    u64 aAbs = a & ABSMASK, bAbs = b & ABSMASK;

    /* NaN / infinity */
    if ((aAbs > INFBITS) || (bAbs > INFBITS))
        return QNANBITS;                            /* a or b is NaN */
    if (aAbs == INFBITS || bAbs == INFBITS) {
        if (aAbs == INFBITS && bAbs == INFBITS) {
            if ((a ^ b) & SIGNBIT)
                return QNANBITS;                    /* +inf + -inf */
            return a;
        }
        return aAbs == INFBITS ? a : b;
    }
    if (aAbs == 0 && bAbs == 0)
        return a & b;                               /* -0 + -0 = -0, else +0 */
    if (aAbs == 0)
        return b;
    if (bAbs == 0)
        return a;

    unpack(a, &aSign, &aExp, &aSig, &aRaw);
    unpack(b, &bSign, &bExp, &bSig, &bRaw);

    if (aSign == bSign) {
        if (aExp >= bExp)
            return add_magnitudes(aSign, aExp, aSig, bExp, bSig);
        return add_magnitudes(aSign, bExp, bSig, aExp, aSig);
    }
    /* different signs -> subtract the smaller magnitude from the larger */
    if (aExp > bExp || (aExp == bExp && aSig >= bSig))
        return sub_magnitudes(aSign, aExp, aSig, bExp, bSig);
    return sub_magnitudes(bSign, bExp, bSig, aExp, aSig);
}

u64
__skj_dsub(u64 a, u64 b)
{
    return __skj_dadd(a, b ^ SIGNBIT);
}

u64
__skj_dmul(u64 a, u64 b)
{
    u32 aSign, bSign, rSign;
    int aExp, bExp, aRaw, bRaw, exp;
    u64 aSig, bSig, hi, lo, sig;
    u64 aAbs = a & ABSMASK, bAbs = b & ABSMASK;

    if ((aAbs > INFBITS) || (bAbs > INFBITS))
        return QNANBITS;
    rSign = (u32)((a ^ b) >> 63) & 1;
    if (aAbs == INFBITS || bAbs == INFBITS) {
        if (aAbs == 0 || bAbs == 0)
            return QNANBITS;                        /* inf * 0 */
        return ((u64)rSign << 63) | INFBITS;
    }
    if (aAbs == 0 || bAbs == 0)
        return (u64)rSign << 63;                    /* signed zero */

    unpack(a, &aSign, &aExp, &aSig, &aRaw);
    unpack(b, &bSign, &bExp, &bSig, &bRaw);
    /* normalize subnormals so each significand has its leading 1 at bit 52 */
    if (!aRaw) {
        int s = clz64(aSig) - (63 - SIGBITS);
        aSig <<= s;
        aExp -= s;
    }
    if (!bRaw) {
        int s = clz64(bSig) - (63 - SIGBITS);
        bSig <<= s;
        bExp -= s;
    }

    exp = aExp + bExp - BIAS;
    /* each significand is in [2^52, 2^53); product is in [2^104, 2^106) */
    umul64(aSig, bSig, &hi, &lo);
    /* bring the 53-bit result with 3 guard bits into the low word.  The
       product's leading 1 is at bit 105 or 104; test the higher bit first,
       since both can be set at once. */
    if (hi & ((u64)1 << (105 - 64))) {              /* bit 105 top */
        sig = (hi << (64 - 50)) | (lo >> 50);       /* keep bits [105..50] */
        sig |= (lo & (((u64)1 << 50) - 1)) != 0;    /* sticky */
        exp++;
    } else {                                        /* bit 104 top */
        sig = (hi << (64 - 49)) | (lo >> 49);       /* keep bits [104..49] */
        sig |= (lo & (((u64)1 << 49) - 1)) != 0;
    }
    return round_pack(rSign, exp, sig);
}

u64
__skj_ddiv(u64 a, u64 b)
{
    u32 aSign, bSign, rSign;
    int aExp, bExp, aRaw, bRaw, exp, i;
    u64 aSig, bSig, q, rem, sig;
    u64 aAbs = a & ABSMASK, bAbs = b & ABSMASK;

    if ((aAbs > INFBITS) || (bAbs > INFBITS))
        return QNANBITS;
    rSign = (u32)((a ^ b) >> 63) & 1;
    if (aAbs == INFBITS) {
        if (bAbs == INFBITS)
            return QNANBITS;                        /* inf / inf */
        return ((u64)rSign << 63) | INFBITS;
    }
    if (bAbs == INFBITS)
        return (u64)rSign << 63;                    /* finite / inf = 0 */
    if (bAbs == 0) {
        if (aAbs == 0)
            return QNANBITS;                        /* 0 / 0 */
        return ((u64)rSign << 63) | INFBITS;        /* x / 0 = inf */
    }
    if (aAbs == 0)
        return (u64)rSign << 63;

    unpack(a, &aSign, &aExp, &aSig, &aRaw);
    unpack(b, &bSign, &bExp, &bSig, &bRaw);
    if (!aRaw) {
        int s = clz64(aSig) - (63 - SIGBITS);
        aSig <<= s;
        aExp -= s;
    }
    if (!bRaw) {
        int s = clz64(bSig) - (63 - SIGBITS);
        bSig <<= s;
        bExp -= s;
    }

    exp = aExp - bExp + BIAS;
    /* Normalize so the ratio lands in [1, 2): then restoring long division
       produces the integer 1 as the first quotient bit, followed by 55
       fraction bits, so the 56-bit quotient already has its leading 1 at bit
       55 (implicit at 52 plus 3 guard bits), the form round_pack wants. */
    if (aSig < bSig) {
        aSig <<= 1;
        exp--;
    }
    q = 0;
    rem = aSig;
    for (i = 0; i < SIGBITS + 4; i++) {             /* 56 iterations */
        q <<= 1;
        if (rem >= bSig) {
            rem -= bSig;
            q |= 1;
        }
        rem <<= 1;
    }
    q |= (rem != 0);                                /* sticky in bit 0 */
    sig = q;
    return round_pack(rSign, exp, sig);
}

/* int -> double, exact (a 32-bit int fits in 53 bits of significand) */
u64
__skj_si2d(s32 x)
{
    u32 sign = 0;
    u64 mag;
    int exp;

    if (x == 0)
        return 0;
    if (x < 0) {
        sign = 1;
        mag = (u64)(-(s32)x) & 0xffffffffULL;
        if (x == (s32)0x80000000)
            mag = 0x80000000ULL;
    } else {
        mag = (u64)x;
    }
    /* place the value with its leading bit at bit 52, then let round_pack (which
       expects 3 guard bits) finish; the value is exact so no rounding occurs. */
    exp = BIAS + SIGBITS;
    while (mag < IMPLICIT) {
        mag <<= 1;
        exp--;
    }
    while (mag >= (IMPLICIT << 1)) {
        mag >>= 1;
        exp++;
    }
    return round_pack(sign, exp, mag << 3);
}

/* double -> int, round toward zero, saturating on overflow */
s32
__skj_d2si(u64 a)
{
    u32 sign;
    int aExp, aRaw, shift;
    u64 aSig, mag;

    unpack(a, &sign, &aExp, &aSig, &aRaw);
    if (aRaw == EXPMAX)                              /* inf or nan */
        return sign ? (s32)0x80000000 : 0x7fffffff;
    if (aRaw == 0)
        return 0;                                   /* subnormal magnitude < 1 */

    shift = aExp - BIAS - SIGBITS;                  /* bit position of the LSB */
    if (aExp - BIAS > 31)                           /* |value| >= 2^31 */
        return sign ? (s32)0x80000000 : 0x7fffffff;
    if (aExp < BIAS)
        return 0;                                   /* |value| < 1 */

    if (shift >= 0)
        mag = aSig << shift;
    else
        mag = aSig >> (-shift);                     /* truncate toward zero */

    if (sign) {
        if (mag > 0x80000000ULL)
            return (s32)0x80000000;
        return -(s32)mag;
    }
    if (mag > 0x7fffffffULL)
        return 0x7fffffff;
    return (s32)mag;
}

/* binary32 bit pattern -> binary64 bit pattern, always exact */
u64
__skj_extendsfdf2(u32 s)
{
    u32 sign = (s >> 31) & 1;
    int exp = (s >> 23) & 0xff;
    u32 frac = s & 0x7fffff;
    u64 dsign = (u64)sign << 63;

    if (exp == 0xff)                                /* inf or nan */
        return dsign | INFBITS | ((u64)frac << 29);
    if (exp == 0) {
        if (frac == 0)
            return dsign;                           /* signed zero */
        /* subnormal single: normalize into a double normal */
        exp = 1;
        while (!(frac & 0x800000)) {
            frac <<= 1;
            exp--;
        }
        frac &= 0x7fffff;
        exp = exp - 127 + BIAS;
        return dsign | ((u64)exp << SIGBITS) | ((u64)frac << 29);
    }
    exp = exp - 127 + BIAS;
    return dsign | ((u64)exp << SIGBITS) | ((u64)frac << 29);
}

/* Round-and-pack for binary32, the single-precision twin of round_pack: sig
 * holds the 24-bit significand (implicit bit at bit 23) shifted left by 3
 * guard bits, leading 1 nominally at bit 26. */
static u32
spack(u32 sign, int exp, u32 sig)
{
    u32 s = sign << 31, round, r;

    if (exp <= 0) {
        int shift = 1 - exp;
        u32 sticky;
        if (shift > 31)
            shift = 31;
        sticky = (sig & ((1u << shift) - 1)) != 0;
        sig = (sig >> shift) | sticky;
        exp = 0;
    }
    round = sig & 7;
    r = sig >> 3;                                   /* 24-bit significand */
    if (round > 4 || (round == 4 && (r & 1)))
        r++;
    if (r & (1u << 24)) {                           /* carried past bit 23 */
        r >>= 1;
        exp++;
    }
    if (exp >= 0xff)
        return s | 0x7f800000;                      /* overflow to infinity */
    if (exp == 0 && (r & (1u << 23)))
        exp = 1;                                    /* subnormal rounded to normal */
    return s | ((u32)exp << 23) | (r & 0x7fffff);
}

/* binary64 bit pattern -> binary32 bit pattern, round to nearest even */
u32
__skj_truncdfsf2(u64 a)
{
    u32 sign;
    int aExp, aRaw, exp;
    u64 aSig;
    u32 sig;

    unpack(a, &sign, &aExp, &aSig, &aRaw);
    if (aRaw == EXPMAX) {                           /* inf or nan */
        if (a & SIGMASK)
            return (sign << 31) | 0x7fc00000;       /* quiet NaN */
        return (sign << 31) | 0x7f800000;
    }
    if (aRaw == 0)
        return sign << 31;                          /* magnitude underflows to zero */

    /* aSig is 53-bit (bit52 = implicit).  A single carries 24 bits plus 3
       guard bits = bit26 implicit, so shift right by 52-26 = 26, folding the
       low 26 discarded bits into a sticky bit at position 0. */
    exp = aExp - BIAS + 127;
    sig = (u32)(aSig >> 26) | ((aSig & (((u64)1 << 26) - 1)) != 0);
    return spack(sign, exp, sig);
}

/* ordered compares: 1 when the relation holds, 0 otherwise (0 when unordered,
 * i.e. a NaN operand, for every relation). */
static int
unordered(u64 a, u64 b)
{
    return (a & ABSMASK) > INFBITS || (b & ABSMASK) > INFBITS;
}

int
__skj_dcmpeq(u64 a, u64 b)
{
    if (unordered(a, b))
        return 0;
    if (((a | b) & ABSMASK) == 0)
        return 1;                                   /* +0 == -0 */
    return a == b;
}

int
__skj_dcmplt(u64 a, u64 b)
{
    int aNeg, bNeg;

    if (unordered(a, b))
        return 0;
    if (((a | b) & ABSMASK) == 0)
        return 0;                                   /* +0 == -0 */
    aNeg = (int)(a >> 63);
    bNeg = (int)(b >> 63);
    if (aNeg != bNeg)
        return aNeg;                                /* negative < positive */
    if (aNeg)
        return a > b;                               /* both negative */
    return a < b;
}

int
__skj_dcmple(u64 a, u64 b)
{
    if (unordered(a, b))
        return 0;
    return __skj_dcmplt(a, b) || __skj_dcmpeq(a, b);
}

/* Single-precision by widening: extend the operands to binary64, run the
 * double core, and round the result to binary32.  Binary64 carries at least
 * 2p+2 bits of a binary32 significand, so this double-rounding is exact for
 * +, -, *, / (it agrees bit-for-bit with a direct single-precision unit), and
 * a compare through the exact widening is exact too.  This is the RISC-V
 * single-precision decision applied here (numbers/doc). */
u32
__skj_fadd(u32 a, u32 b)
{
    return __skj_truncdfsf2(__skj_dadd(__skj_extendsfdf2(a),
                                       __skj_extendsfdf2(b)));
}

u32
__skj_fsub(u32 a, u32 b)
{
    return __skj_truncdfsf2(__skj_dsub(__skj_extendsfdf2(a),
                                       __skj_extendsfdf2(b)));
}

u32
__skj_fmul(u32 a, u32 b)
{
    return __skj_truncdfsf2(__skj_dmul(__skj_extendsfdf2(a),
                                       __skj_extendsfdf2(b)));
}

u32
__skj_fdiv(u32 a, u32 b)
{
    return __skj_truncdfsf2(__skj_ddiv(__skj_extendsfdf2(a),
                                       __skj_extendsfdf2(b)));
}

int
__skj_fcmpeq(u32 a, u32 b)
{
    return __skj_dcmpeq(__skj_extendsfdf2(a), __skj_extendsfdf2(b));
}

int
__skj_fcmplt(u32 a, u32 b)
{
    return __skj_dcmplt(__skj_extendsfdf2(a), __skj_extendsfdf2(b));
}

int
__skj_fcmple(u32 a, u32 b)
{
    return __skj_dcmple(__skj_extendsfdf2(a), __skj_extendsfdf2(b));
}

u32
__skj_si2f(int x)
{
    return __skj_truncdfsf2(__skj_si2d(x));
}

int
__skj_f2si(u32 a)
{
    return __skj_d2si(__skj_extendsfdf2(a));
}
