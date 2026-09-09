/* test_softfloat.c : host-only unit test for runtime/softfloat.c.
 *
 * The soft-float library reproduces IEEE-754 binary64 with integer math for a
 * target with no FPU.  This test uses the host's own hardware double as the
 * oracle: for a wide table of operand pairs it computes the result both ways
 * and compares the raw bit patterns.  A mismatch is a rounding or edge-case
 * bug in the library, caught here on the host without any cross toolchain or
 * emulator.  It is the counterpart of test-gc: pure host C, no target.
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;

extern u64 __skj_dadd(u64, u64);
extern u64 __skj_dsub(u64, u64);
extern u64 __skj_dmul(u64, u64);
extern u64 __skj_ddiv(u64, u64);
extern u64 __skj_si2d(int);
extern int __skj_d2si(u64);
extern u64 __skj_extendsfdf2(u32);
extern u32 __skj_truncdfsf2(u64);
extern int __skj_dcmpeq(u64, u64);
extern int __skj_dcmplt(u64, u64);
extern int __skj_dcmple(u64, u64);
extern u32 __skj_fadd(u32, u32);
extern u32 __skj_fsub(u32, u32);
extern u32 __skj_fmul(u32, u32);
extern u32 __skj_fdiv(u32, u32);
extern int __skj_fcmpeq(u32, u32);
extern int __skj_fcmplt(u32, u32);
extern int __skj_fcmple(u32, u32);
extern u32 __skj_si2f(int);
extern int __skj_f2si(u32);

static u64
d2b(double d)
{
    u64 b;
    memcpy(&b, &d, 8);
    return b;
}

static u32
f2b(float f)
{
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}

static int fails;

/* Compare two binary64 bit patterns.  Any two NaNs count as equal (the
 * library emits one canonical quiet NaN; the payload is not part of the ABI). */
static int
eq64(u64 x, u64 y)
{
    int xn = ((x & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) &&
             (x & 0x000fffffffffffffULL);
    int yn = ((y & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL) &&
             (y & 0x000fffffffffffffULL);
    if (xn && yn)
        return 1;
    return x == y;
}

static int
eq32(u32 x, u32 y)
{
    int xn = ((x & 0x7f800000u) == 0x7f800000u) && (x & 0x7fffffu);
    int yn = ((y & 0x7f800000u) == 0x7f800000u) && (y & 0x7fffffu);
    if (xn && yn)
        return 1;
    return x == y;
}

static void
check_bin(const char *name, double a, double b, u64 got, double want)
{
    u64 w = d2b(want);
    if (!eq64(got, w)) {
        fails++;
        printf("FAIL %s(%.17g, %.17g): got %016llx want %016llx\n",
               name, a, b, (unsigned long long)got, (unsigned long long)w);
    }
}

int
main(void)
{
    static const double vals[] = {
        0.0, -0.0, 1.0, -1.0, 2.0, 0.5, -0.5, 3.0, 4.0, 10.0, -3.0,
        0.1, -0.1, 0.2, 0.3, 1.0 / 3.0, 2.0 / 3.0, 123456.789, -987654.321,
        1e300, 1e-300, 1e-310 /* subnormal */, 3.14159265358979,
        2.718281828459045, 1.5, 2.5, 0.25, 100.0, 1e16, 1e17,
        9007199254740992.0 /* 2^53 */, 9007199254740993.0 /* not representable */,
        1.7976931348623157e308 /* DBL_MAX */, 4.9406564584124654e-324 /* denorm min */,
    };
    int n = (int)(sizeof(vals) / sizeof(vals[0]));
    double inf = HUGE_VAL, nan = NAN;
    int i, j;

    /* arithmetic over all ordered pairs */
    for (i = 0; i < n; i++) {
        for (j = 0; j < n; j++) {
            double a = vals[i], b = vals[j];
            u64 ab = d2b(a), bb = d2b(b);
            check_bin("dadd", a, b, __skj_dadd(ab, bb), a + b);
            check_bin("dsub", a, b, __skj_dsub(ab, bb), a - b);
            check_bin("dmul", a, b, __skj_dmul(ab, bb), a * b);
            check_bin("ddiv", a, b, __skj_ddiv(ab, bb), a / b);
        }
    }

    /* specials */
    {
        double sp[] = { inf, -inf, nan, 0.0, -0.0, 1.0, -1.0, 1e300, -1e300 };
        int m = (int)(sizeof(sp) / sizeof(sp[0]));
        for (i = 0; i < m; i++)
            for (j = 0; j < m; j++) {
                double a = sp[i], b = sp[j];
                u64 ab = d2b(a), bb = d2b(b);
                check_bin("dadd", a, b, __skj_dadd(ab, bb), a + b);
                check_bin("dsub", a, b, __skj_dsub(ab, bb), a - b);
                check_bin("dmul", a, b, __skj_dmul(ab, bb), a * b);
                check_bin("ddiv", a, b, __skj_ddiv(ab, bb), a / b);
            }
    }

    /* int <-> double */
    {
        int ints[] = { 0, 1, -1, 2, -2, 42, -42, 1000000, -1000000,
                       2147483647, -2147483648, 65536, -65536, 123456789 };
        int m = (int)(sizeof(ints) / sizeof(ints[0]));
        for (i = 0; i < m; i++) {
            u64 got = __skj_si2d(ints[i]);
            u64 w = d2b((double)ints[i]);
            if (got != w) {
                fails++;
                printf("FAIL si2d(%d): got %016llx want %016llx\n",
                       ints[i], (unsigned long long)got, (unsigned long long)w);
            }
        }
        /* d2si, round toward zero, over in-range values */
        for (i = 0; i < n; i++) {
            double d = vals[i];
            if (d >= -2147483648.0 && d < 2147483648.0) {
                int got = __skj_d2si(d2b(d));
                int w = (int)d;
                if (got != w) {
                    fails++;
                    printf("FAIL d2si(%.17g): got %d want %d\n", d, got, w);
                }
            }
        }
        /* saturation */
        if (__skj_d2si(d2b(1e300)) != 2147483647) { fails++; printf("FAIL d2si +sat\n"); }
        if (__skj_d2si(d2b(-1e300)) != (int)0x80000000) { fails++; printf("FAIL d2si -sat\n"); }
    }

    /* single <-> double */
    {
        float fs[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.14159f, 1e30f, 1e-30f,
                       1e-40f /* subnormal single */, 2.5f, 123456.7f };
        int m = (int)(sizeof(fs) / sizeof(fs[0]));
        for (i = 0; i < m; i++) {
            u64 got = __skj_extendsfdf2(f2b(fs[i]));
            u64 w = d2b((double)fs[i]);
            if (got != w) {
                fails++;
                printf("FAIL extendsfdf2(%g): got %016llx want %016llx\n",
                       fs[i], (unsigned long long)got, (unsigned long long)w);
            }
        }
        for (i = 0; i < n; i++) {
            u32 got = __skj_truncdfsf2(d2b(vals[i]));
            u32 w = f2b((float)vals[i]);
            if (!eq32(got, w)) {
                fails++;
                printf("FAIL truncdfsf2(%.17g): got %08x want %08x\n",
                       vals[i], got, w);
            }
        }
        /* single overflow / underflow from double */
        if (__skj_truncdfsf2(d2b(1e300)) != 0x7f800000u) { fails++; printf("FAIL trunc +inf\n"); }
        if (__skj_truncdfsf2(d2b(-1e300)) != 0xff800000u) { fails++; printf("FAIL trunc -inf\n"); }
    }

    /* compares */
    for (i = 0; i < n; i++)
        for (j = 0; j < n; j++) {
            double a = vals[i], b = vals[j];
            u64 ab = d2b(a), bb = d2b(b);
            if (__skj_dcmpeq(ab, bb) != (a == b)) { fails++; printf("FAIL cmpeq %.17g %.17g\n", a, b); }
            if (__skj_dcmplt(ab, bb) != (a < b)) { fails++; printf("FAIL cmplt %.17g %.17g\n", a, b); }
            if (__skj_dcmple(ab, bb) != (a <= b)) { fails++; printf("FAIL cmple %.17g %.17g\n", a, b); }
        }
    /* NaN compares are all false */
    {
        u64 nb = d2b(nan);
        if (__skj_dcmpeq(nb, nb) || __skj_dcmplt(nb, nb) || __skj_dcmple(nb, nb)) {
            fails++; printf("FAIL nan compares\n");
        }
        if (__skj_dcmpeq(nb, d2b(1.0)) || __skj_dcmplt(nb, d2b(1.0))) {
            fails++; printf("FAIL nan vs 1 compares\n");
        }
    }

    /* single-precision wrappers, checked against native float ops (which the
       double-then-round path reproduces exactly for +,-,*,/) */
    {
        float fs[] = { 0.0f, -0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 3.0f, 4.0f,
                       10.0f, 0.1f, 0.3f, 1.0f / 3.0f, 123456.7f, -987.654f,
                       1e30f, 1e-30f, 2.5f, 0.25f };
        int m = (int)(sizeof(fs) / sizeof(fs[0]));
        for (i = 0; i < m; i++)
            for (j = 0; j < m; j++) {
                float a = fs[i], b = fs[j];
                u32 ab = f2b(a), bb = f2b(b);
                if (!eq32(__skj_fadd(ab, bb), f2b(a + b))) { fails++; printf("FAIL fadd %g %g\n", a, b); }
                if (!eq32(__skj_fsub(ab, bb), f2b(a - b))) { fails++; printf("FAIL fsub %g %g\n", a, b); }
                if (!eq32(__skj_fmul(ab, bb), f2b(a * b))) { fails++; printf("FAIL fmul %g %g\n", a, b); }
                if (!eq32(__skj_fdiv(ab, bb), f2b(a / b))) { fails++; printf("FAIL fdiv %g %g\n", a, b); }
                if (__skj_fcmpeq(ab, bb) != (a == b)) { fails++; printf("FAIL fcmpeq %g %g\n", a, b); }
                if (__skj_fcmplt(ab, bb) != (a < b)) { fails++; printf("FAIL fcmplt %g %g\n", a, b); }
                if (__skj_fcmple(ab, bb) != (a <= b)) { fails++; printf("FAIL fcmple %g %g\n", a, b); }
            }
        {
            int ints[] = { 0, 1, -1, 42, -42, 1000000, 16777216, 16777217 };
            int p, q2 = (int)(sizeof(ints) / sizeof(ints[0]));
            for (p = 0; p < q2; p++) {
                if (__skj_si2f(ints[p]) != f2b((float)ints[p])) { fails++; printf("FAIL si2f %d\n", ints[p]); }
            }
            for (p = 0; p < m; p++) {
                float d = fs[p];
                if (d >= -2147483648.0f && d < 2147483648.0f && __skj_f2si(f2b(d)) != (int)d) {
                    fails++; printf("FAIL f2si %g\n", d);
                }
            }
        }
    }

    if (fails) {
        printf("softfloat: %d FAILED\n", fails);
        return 1;
    }
    printf("softfloat: all checks passed\n");
    return 0;
}
