/* input_sound.c - the milestone-2 libpsx sample.
 *
 * Reads the controller each frame and moves a square with the d-pad, and plays
 * a tone through the SPU: once at startup, and again on each press of Cross.
 * It exercises every milestone-2 path (GPU, DMA, vsync, pad, SPU upload and
 * play) and exits cleanly after a fixed number of frames.
 *
 * On a headless PCSX-Redux run no buttons are pressed, so the square stays put;
 * the clean exit confirms the pad poll and the SPU setup run without hanging.
 * On real hardware the d-pad moves the square and Cross retriggers the tone.
 *
 * The tone sample (psx_tone) is generated at build time by vagconv and linked
 * in. It is uploaded to sound RAM at 0x1010, just above the reserved area.
 */

#include "psx.h"

extern const unsigned char psx_tone[];
extern const unsigned psx_tone_len;

#define SQUARE 24
#define FRAMES 300
#define TONE_ADDR 0x1010u

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();
    psx_spu_init();
    psx_spu_upload(TONE_ADDR, psx_tone, psx_tone_len);

    psx_set_bg(0, 0, 40);

    int x = (PSX_SCREEN_W - SQUARE) / 2;
    int y = (PSX_SCREEN_H - SQUARE) / 2;
    int prev_cross = 0;

    psx_spu_play(0, TONE_ADDR, PSX_SPU_PITCH_22050, 0x3FFF);  /* startup tone */

    for (int f = 0; f < FRAMES; f++) {
        uint16_t b = psx_pad_read();

        if (b & PSX_PAD_LEFT)
            x -= 2;
        if (b & PSX_PAD_RIGHT)
            x += 2;
        if (b & PSX_PAD_UP)
            y -= 2;
        if (b & PSX_PAD_DOWN)
            y += 2;
        if (x < 0)
            x = 0;
        if (x > PSX_SCREEN_W - SQUARE)
            x = PSX_SCREEN_W - SQUARE;
        if (y < 0)
            y = 0;
        if (y > PSX_SCREEN_H - SQUARE)
            y = PSX_SCREEN_H - SQUARE;

        int cross = (b & PSX_PAD_CROSS) != 0;
        if (cross && !prev_cross)
            psx_spu_play(0, TONE_ADDR, PSX_SPU_PITCH_22050, 0x3FFF);
        prev_cross = cross;

        psx_draw_rect(x, y, SQUARE, SQUARE, 255, 200, 0);
        psx_vsync();
        psx_display_swap();
    }

    return 0;
}
