/* env_tone.c - a tone shaped by an ADSR envelope, with reverb.
 *
 * Two things are checked here. First, a runtime self-check: psx_spu_make_adsr
 * must pack the chosen envelope into the exact ADSR words the hardware expects.
 * A mismatch exits non-zero, so the command tier catches a regression in the
 * bit layout. Second, the tone still has to reach the output: the voice is keyed
 * on through psx_spu_play_adsr (the envelope path, not the fixed default) with
 * reverb enabled, so the capture tier confirms a 440 Hz tone comes out of the
 * SPU with those paths exercised.
 */

#include "psx.h"

extern const unsigned char psx_atone[];
extern const unsigned      psx_atone_len;

#define TONE_ADDR 0x1010
#define FRAMES 120

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();
    psx_spu_init();

    /* attack 2, decay 4, sustain 12, release 6. The expected packing:
     *   ADSR1 = (4<<10) | (4<<4) | 12                 = 0x104C
     *   ADSR2 = (1<<15)|(1<<14)|(0x1F<<8)|(1<<5)|12    = 0xDF2C
     */
    uint32_t adsr = psx_spu_make_adsr(2, 4, 12, 6);
    if (adsr != 0xDF2C104Cu)
        return 1;

    psx_spu_upload(TONE_ADDR, psx_atone, (uint32_t)psx_atone_len);
    psx_spu_reverb(PSX_REVERB_HALL);
    psx_spu_voice_reverb(0, 1);
    psx_spu_play_adsr(0, TONE_ADDR, PSX_SPU_PITCH_22050, 0x3FFF, adsr);

    for (int f = 0; f < FRAMES; f++) {
        psx_vsync();
        psx_display_swap();
    }

    psx_spu_stop(0);
    psx_spu_reverb_off();
    return 0;
}
