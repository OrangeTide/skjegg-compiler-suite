/* alpha.c - semi-transparent drawing.
 *
 * Draws an opaque red rectangle and a semi-transparent blue rectangle that
 * overlaps it. Where they overlap the GPU blends the two, so the overlap reads
 * as a mix of red and blue. This is the fade/flash/HUD-dimming primitive.
 *
 * The framebuffer check reads the opaque red where only red is drawn and the
 * blended color where the two overlap, so the blend actually happened.
 */

#include "psx.h"

#define FRAMES 120

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 20);

    for (int f = 0; f < FRAMES; f++) {
        psx_draw_rect(100, 100, 60, 60, 255, 0, 0);            /* opaque red */
        psx_draw_rect_alpha(130, 100, 60, 60, 0, 0, 255, 0);   /* half blue over it */
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
