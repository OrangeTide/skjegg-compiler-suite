/* gte_mat.c - rotation-matrix builders for the GTE.
 *
 * The GTE takes a 3x3 rotation matrix in 1.3.12 fixed point (4096 = 1.0). This
 * builds one from an angle using the libpsx fixed-point trig, so a 3D program
 * spins an object without doing the trig itself. The trig returns 16.16, which
 * is shifted right by 4 to the matrix's 12 fractional bits.
 */

#include "psx.h"

void
psx_gte_rotmat_y(short *m, int angle)
{
    short c = (short)(psx_fcos(angle) >> 4);
    short s = (short)(psx_fsin(angle) >> 4);

    /* Rotation about the Y axis:
     *   [  c  0  s ]
     *   [  0  1  0 ]
     *   [ -s  0  c ]  */
    m[0] = c;    m[1] = 0;    m[2] = s;
    m[3] = 0;    m[4] = 4096; m[5] = 0;
    m[6] = (short)-s; m[7] = 0; m[8] = c;
}

void
psx_gte_rotmat_x(short *m, int angle)
{
    short c = (short)(psx_fcos(angle) >> 4);
    short s = (short)(psx_fsin(angle) >> 4);

    /* Rotation about the X axis:
     *   [ 1  0  0 ]
     *   [ 0  c -s ]
     *   [ 0  s  c ]  */
    m[0] = 4096; m[1] = 0;        m[2] = 0;
    m[3] = 0;    m[4] = c;        m[5] = (short)-s;
    m[6] = 0;    m[7] = s;        m[8] = c;
}

void
psx_gte_rotmat_z(short *m, int angle)
{
    short c = (short)(psx_fcos(angle) >> 4);
    short s = (short)(psx_fsin(angle) >> 4);

    /* Rotation about the Z axis:
     *   [ c -s  0 ]
     *   [ s  c  0 ]
     *   [ 0  0  1 ]  */
    m[0] = c;    m[1] = (short)-s; m[2] = 0;
    m[3] = s;    m[4] = c;         m[5] = 0;
    m[6] = 0;    m[7] = 0;         m[8] = 4096;
}

void
psx_gte_matmul(short *out, const short *a, const short *b)
{
    /* out = a * b, all 3x3 in 1.3.12 fixed point. The product of two 1.3.12
     * values is 1.6.24, so each term is shifted back down by 12. A separate
     * output lets callers compose in place via a temporary. */
    short t[9];
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int acc = 0;
            for (int k = 0; k < 3; k++)
                acc += (int)a[r * 3 + k] * (int)b[k * 3 + col];
            t[r * 3 + col] = (short)(acc >> 12);
        }
    }
    for (int i = 0; i < 9; i++)
        out[i] = t[i];
}
