/* hello_gpu.c - the milestone-1 libpsx sample.
 *
 * Brings up the GPU and animates a flat square across the screen on a
 * double-buffered, vsync-paced loop, then exits. This is the first program
 * that drives the PlayStation display through libpsx, and the regression
 * anchor the Pascal and Kobold ports will mirror.
 *
 * Build: compiled with skj-cc-mips-sf, linked with start_psx.o and libpsx.a
 * into a PS-EXE (see the test-psx-gpu Makefile target). It runs on the
 * PCSX-Redux emulator with a real BIOS; exiting returns 0, which the emulator
 * reports in test mode.
 */

#include "psx.h"

#define SQUARE 32
#define FRAMES 180      /* about three seconds at 60 Hz, then exit */

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();                     /* interrupt-driven vsync */
    psx_set_bg(0, 0, 40);               /* dark blue field */

    int x = 0;
    int dx = 2;

    for (int f = 0; f < FRAMES; f++) {
        psx_draw_rect(x, (PSX_SCREEN_H - SQUARE) / 2, SQUARE, SQUARE,
                      255, 200, 0);     /* a yellow square */
        psx_vsync();
        psx_display_swap();

        x += dx;
        if (x <= 0 || x >= PSX_SCREEN_W - SQUARE)
            dx = -dx;
    }

    return 0;
}
