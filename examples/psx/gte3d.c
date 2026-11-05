/* gte3d.c - self-check for the GTE triangle, culling, and depth ops.
 *
 * Builds on gte_test (which checks single-vertex projection) to verify the
 * three-at-once transform (RTPT), the backface cross product (NCLIP), the
 * ordering-table depth (AVSZ3), and the X/Z rotation matrices and 3x3 compose.
 * With H equal to the eye distance the projection is 1:1, so screen positions
 * are known from the arithmetic. Distinct nonzero exit codes name the failed
 * check; 0 means the 3D math is correct.
 */

#include "psx.h"

static int
near(int a, int b)
{
    int d = a - b;
    return d >= -1 && d <= 1;
}

int
main(void)
{
    short id[9] = { 4096, 0, 0,  0, 4096, 0,  0, 0, 4096 };
    int out6[6], out[3];

    psx_gte_set_rot(id);
    psx_gte_set_trans(0, 0, 300);
    psx_gte_set_screen(300, 160, 120);

    /* RTPT a triangle: (0,-40,0),(40,40,0),(-40,40,0) -> 1:1 screen coords. */
    short tri[9] = { 0, -40, 0,  40, 40, 0,  -40, 40, 0 };
    psx_gte_rtpt(tri, out6);
    if (!near(out6[0], 160) || !near(out6[1], 80))  return 10;  /* apex */
    if (!near(out6[2], 200) || !near(out6[3], 160)) return 11;  /* lower right */
    if (!near(out6[4], 120) || !near(out6[5], 160)) return 12;  /* lower left */

    /* NCLIP: this winding gives one sign; the reversed winding the other. */
    int area = psx_gte_nclip();
    if (area == 0) return 13;

    short rev[9] = { 0, -40, 0,  -40, 40, 0,  40, 40, 0 };
    psx_gte_rtpt(rev, out6);
    int area2 = psx_gte_nclip();
    if ((area > 0) == (area2 > 0)) return 14;      /* must flip sign */

    /* AVSZ3: average depth of the triangle (all at z=300 -> translated 300),
     * scaled by ZSF3. It must be positive and grow when the triangle is
     * pushed further away. */
    psx_gte_rtpt(tri, out6);
    int otz_near = psx_gte_avsz3();
    if (otz_near <= 0) return 15;

    psx_gte_set_trans(0, 0, 600);
    psx_gte_rtpt(tri, out6);
    int otz_far = psx_gte_avsz3();
    if (otz_far <= otz_near) return 16;            /* farther -> larger OTZ */

    /* X rotation by a quarter turn maps +y to +z (out of view) and keeps the
     * eye looking at a point whose y-projection moves toward center. Check a
     * point on +y projects near the center x and above center y with the X
     * matrix applied: (0,40,0) rotated 90 deg about X -> (0,0,40). */
    psx_gte_set_trans(0, 0, 300);
    short mx[9];
    psx_gte_rotmat_x(mx, 256);              /* 256/1024 = quarter turn */
    psx_gte_set_rot(mx);
    psx_gte_rtps(0, 40, 0, out);            /* -> about (160,120), depth ~340 */
    if (!near(out[0], 160)) return 20;
    if (!near(out[1], 120)) return 21;
    if (out[2] < 320 || out[2] > 360) return 22;   /* z gained ~40 */

    /* Z rotation by a quarter turn maps +x to +y: (40,0,0) -> (0,40,0). */
    short mz[9];
    psx_gte_rotmat_z(mz, 256);
    psx_gte_set_rot(mz);
    psx_gte_rtps(40, 0, 0, out);            /* -> (160, 160) */
    if (!near(out[0], 160)) return 30;
    if (!near(out[1], 160)) return 31;

    /* Compose: X then Z. Applying the product to (40,0,0) should match applying
     * Z alone here (X leaves x unchanged for a point with y=z=0). */
    short mc[9];
    psx_gte_matmul(mc, mz, mx);
    psx_gte_set_rot(mc);
    psx_gte_rtps(40, 0, 0, out);
    if (!near(out[0], 160)) return 40;
    if (!near(out[1], 160)) return 41;

    return 0;
}
