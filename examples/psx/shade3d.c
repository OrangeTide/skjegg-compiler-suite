/* shade3d.c - a Gouraud-shaded quad through the ordering table.
 *
 * Draws one large Gouraud quad whose four corners are red, green, blue, and
 * white; the GPU interpolates the colors across it. The corners of the
 * framebuffer then read close to the corner colors, which the capture tier
 * checks, verifying per-vertex shading (GP0 0x38) and that a primitive drawn
 * through the ordering table reaches the screen.
 */

#include "psx.h"

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 0);

    for (int f = 0; f < 120; f++) {
        psx_ot_clear();
        /* GPU quad order v0,v1,v2,v3 = TL, TR, BL, BR. */
        psx_ot_add_gouraud4(40, 40, 0xFF0000,       /* TL red   */
                            280, 40, 0x00FF00,      /* TR green */
                            40, 200, 0x0000FF,      /* BL blue  */
                            280, 200, 0xFFFFFF,     /* BR white */
                            1);
        psx_ot_link();
        psx_vsync();
        psx_display_swap();
    }
    return 0;
}
