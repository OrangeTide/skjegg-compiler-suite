/* gte_test.c - a self-checking GTE projection test.
 *
 * Sets a known transform (identity rotation, a translation down the z axis, and
 * a centered projection) and projects a few vertices whose screen positions are
 * known from the perspective arithmetic. It exits 0 only if the GTE produced
 * them, so the exit-code tier verifies the coprocessor-2 math itself, the way
 * the FPU and i64 self-tests check arithmetic. Distinct nonzero codes name the
 * vertex that was wrong.
 *
 * With H = 300 and the eye-to-object distance (translation z) also 300, the
 * projection is 1:1: a point at model x maps to screen x = 160 + x, and model y
 * to screen y = 120 + y.
 */

#include "psx.h"

static int
near(int a, int b)
{
    int d = a - b;
    return d >= -1 && d <= 1;           /* allow a one-pixel rounding step */
}

int
main(void)
{
    int out[3];
    short m[9] = { 4096, 0, 0,  0, 4096, 0,  0, 0, 4096 };  /* identity */

    psx_gte_set_rot(m);
    psx_gte_set_trans(0, 0, 300);
    psx_gte_set_screen(300, 160, 120);

    psx_gte_rtps(100, 0, 0, out);               /* -> (260, 120) */
    if (!near(out[0], 260)) return 10;
    if (!near(out[1], 120)) return 11;

    psx_gte_rtps(0, 50, 0, out);                /* -> (160, 170) */
    if (!near(out[0], 160)) return 12;
    if (!near(out[1], 170)) return 13;

    psx_gte_rtps(-80, -40, 0, out);             /* -> (80, 80) */
    if (!near(out[0], 80)) return 14;
    if (!near(out[1], 80)) return 15;

    /* Depth: the translated z is 300, so SZ3 is about 300. */
    psx_gte_rtps(0, 0, 0, out);
    if (out[2] < 290 || out[2] > 310) return 16;

    return 0;
}
