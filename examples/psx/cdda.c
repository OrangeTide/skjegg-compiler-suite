/* cdda.c - CD audio command self-check (CD-DA and XA).
 *
 * Headless PCSX-Redux routes CD-DA audio to the host sound device, not through
 * the SPU output the capture tier records, so this does not measure a tone (a
 * host monitor capture confirms the sound separately). What it verifies, by
 * exit code, is that the CD-audio command path drives the controller correctly
 * against a real two-track disc: the table of contents reports the audio track,
 * a CD-DA track plays with an acknowledgement, and the XA streaming commands are
 * accepted. Distinct nonzero codes name the step that failed.
 *
 * The disc (mkcdda) has a data track and a Red Book audio track, so a game's
 * whole CD-audio sequence is exercised: enable the SPU CD input, read the TOC,
 * play a track, and start an XA stream.
 */

#include "psx.h"

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();
    psx_spu_init();
    psx_cd_init();

    psx_cd_audio_enable(0x6000);        /* route the CD input into the SPU mix */

    /* The disc has a data track and an audio track. */
    if (psx_cd_track_count() != 2)
        return 10;

    /* Play the Red Book audio track; the drive acknowledges the command. */
    if (psx_cd_play_track(2) != 0)
        return 11;
    for (int f = 0; f < 30; f++)
        psx_vsync();
    psx_cd_audio_stop();

    /* Start an XA-ADPCM stream; the filter/mode/seek/read commands are
     * accepted. (This disc carries no XA audio, so nothing decodes; the point
     * is that the streaming command path drives the controller.) */
    if (psx_cd_play_xa(0, 0, 0) != 0)
        return 12;
    for (int f = 0; f < 10; f++)
        psx_vsync();
    psx_cd_audio_stop();

    return 0;
}
