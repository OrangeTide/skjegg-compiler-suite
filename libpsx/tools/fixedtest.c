/* fixedtest.c - host self-test for libpsx fixed-point math (milestone 4).
 *
 * Compiles the target's fixed.c and the generated trig table on the host and
 * checks psx_fmul, psx_fdiv, psx_fsin, and psx_fcos against double-precision
 * math. Host-only, no emulator (the test-softfloat / test-psx-adpcm pattern).
 */

#include <math.h>
#include <stdio.h>

#include "psx.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int fails = 0;

static void
chk(const char *what, double got, double want, double tol)
{
    double e = got - want;
    if (e < 0)
        e = -e;
    if (e > tol) {
        printf("FAIL %s: got %.5f want %.5f\n", what, got, want);
        fails++;
    }
}

static double
f2d(fix16 f)
{
    return (double)f / 65536.0;
}

int
main(void)
{
    chk("fmul 1.5*2", f2d(psx_fmul(PSX_FIX(3) / 2, PSX_FIX(2))), 3.0, 0.001);
    chk("fmul -1.25*4", f2d(psx_fmul(-(PSX_FIX(5) / 4), PSX_FIX(4))), -5.0, 0.001);
    chk("fdiv 7/2", f2d(psx_fdiv(PSX_FIX(7), PSX_FIX(2))), 3.5, 0.001);
    chk("fdiv 1/3", f2d(psx_fdiv(PSX_FIX(1), PSX_FIX(3))), 1.0 / 3.0, 0.001);
    chk("fdiv -9/4", f2d(psx_fdiv(PSX_FIX(-9), PSX_FIX(4))), -2.25, 0.001);

    for (int a = 0; a < 1024; a += 17) {
        chk("fsin", f2d(psx_fsin(a)), sin(2.0 * M_PI * a / 1024.0), 0.001);
        chk("fcos", f2d(psx_fcos(a)), cos(2.0 * M_PI * a / 1024.0), 0.001);
    }

    if (fails == 0)
        printf("fixedtest: all checks passed\n");
    return fails ? 1 : 0;
}
