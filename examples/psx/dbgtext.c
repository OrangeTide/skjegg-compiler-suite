/* dbgtext.c - the built-in debug-text overlay.
 *
 * Prints a few lines of white text on a dark background each frame. The capture
 * tier then checks that white glyph pixels reached the framebuffer (the text
 * drew) and that a corner well away from the text stayed the background colour
 * (the text did not flood the screen), verifying the font upload and the
 * per-character textured-rectangle draw.
 */

#include "psx.h"

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();
    psx_set_bg(0, 0, 16);
    psx_debug_init();

    for (int f = 0; f < 120; f++) {
        psx_debug_print(80, 100, "HELLO PLAYSTATION");
        psx_debug_print(80, 112, "debug text 0123456789");
        psx_debug_print(80, 124, "ABCxyz !?+-=/*");
        psx_vsync();
        psx_display_swap();
    }
    return 0;
}
