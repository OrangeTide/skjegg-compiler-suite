/* gte_easy.c - pointer-free GTE helpers for the language units.
 *
 * The core GTE API (gte.S, gte_mat.c) passes matrices and vertex arrays by
 * pointer, which C uses directly but Compact Pascal (no pointers) and the Kobold
 * unit (kept pointer-free) cannot. These helpers keep the matrix and the last
 * projected point on the C side and expose only scalar-in, scalar-out calls, the
 * same way the texture handles keep the TIM pointer on the C side. A Pascal or
 * Kobold 3D program sets the view once, builds a rotation from three angles,
 * projects each vertex, and reads back its screen x, y, and depth.
 */

#include "psx.h"

static int proj_x, proj_y, proj_z;

void
psx_gte_rot_xyz(int ax, int ay, int az)
{
    short mx[9], my[9], mz[9], t[9], r[9];

    /* Compose the three axis rotations into one matrix and load it. */
    psx_gte_rotmat_x(mx, ax);
    psx_gte_rotmat_y(my, ay);
    psx_gte_rotmat_z(mz, az);
    psx_gte_matmul(t, mx, my);
    psx_gte_matmul(r, t, mz);
    psx_gte_set_rot(r);
}

void
psx_gte_project(int vx, int vy, int vz)
{
    int out[3];

    psx_gte_rtps(vx, vy, vz, out);
    proj_x = out[0];
    proj_y = out[1];
    proj_z = out[2];
}

int
psx_gte_sx(void)
{
    return proj_x;
}

int
psx_gte_sy(void)
{
    return proj_y;
}

int
psx_gte_sz(void)
{
    return proj_z;
}
