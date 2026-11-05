/* cube.c - a spinning 3D cube through the full GTE pipeline.
 *
 * The milestone-8 sample, now using the whole 3D path: each frame builds a
 * composed X and Y rotation, and for every face projects three corners with one
 * RTPT, culls back faces with NCLIP (the screen-space cross product), takes the
 * face depth from AVSZ3, projects the fourth corner, and adds the quad to the
 * ordering table at its depth bucket. One psx_display_swap then draws the frame
 * clear and the depth-sorted faces.
 *
 * This is the GTE transforming and projecting, hardware backface culling, and
 * an ordering table drawing back to front, feeding the same flat-quad primitive
 * the 2D path uses.
 */

#include "psx.h"

#define S 40
#define FRAMES 180

static const short V[8][3] = {
    { -S, -S, -S }, {  S, -S, -S }, {  S,  S, -S }, { -S,  S, -S },
    { -S, -S,  S }, {  S, -S,  S }, {  S,  S,  S }, { -S,  S,  S },
};

/* Each face: four corner indices wound counterclockwise seen from outside, and
 * a color. NCLIP's sign then tells which faces point at the camera. */
static const unsigned char F[6][4] = {
    { 4, 5, 6, 7 },             /* front  (+z) */
    { 1, 0, 3, 2 },             /* back   (-z) */
    { 0, 4, 7, 3 },             /* left   (-x) */
    { 5, 1, 2, 6 },             /* right  (+x) */
    { 0, 1, 5, 4 },             /* top    (-y) */
    { 3, 7, 6, 2 },             /* bottom (+y) */
};
static const unsigned char C[6][3] = {
    { 255, 0, 0 }, { 0, 255, 0 }, { 0, 0, 255 },
    { 255, 255, 0 }, { 255, 0, 255 }, { 0, 255, 255 },
};

int
main(void)
{
    short my[9], mx[9], m[9];
    int angle = 0;

    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 20);
    psx_gte_set_screen(300, 160, 120);

    for (int f = 0; f < FRAMES; f++) {
        psx_gte_rotmat_y(my, angle);
        psx_gte_rotmat_x(mx, angle / 2);
        psx_gte_matmul(m, my, mx);
        psx_gte_set_rot(m);
        psx_gte_set_trans(0, 0, 220);

        psx_ot_clear();

        for (int fi = 0; fi < 6; fi++) {
            int a = F[fi][0], b = F[fi][1], c = F[fi][2], d = F[fi][3];

            /* Project three corners at once; NCLIP and AVSZ3 then read the GTE
             * FIFO those left behind. */
            short tri[9] = {
                V[a][0], V[a][1], V[a][2],
                V[b][0], V[b][1], V[b][2],
                V[c][0], V[c][1], V[c][2],
            };
            int s[6];
            psx_gte_rtpt(tri, s);

            if (psx_gte_nclip() <= 0)       /* back face: skip */
                continue;
            int z = psx_gte_avsz3();

            int s3[3];
            psx_gte_rtps(V[d][0], V[d][1], V[d][2], s3);

            /* Corners a,b,c,d cyclic -> GPU quad order a,b,d,c. */
            psx_ot_add_poly4(s[0], s[1], s[2], s[3], s3[0], s3[1], s[4], s[5],
                             C[fi][0], C[fi][1], C[fi][2], z);
        }

        psx_ot_link();
        psx_vsync();
        psx_display_swap();
        angle = (angle + 4) & 1023;
    }

    return 0;
}
