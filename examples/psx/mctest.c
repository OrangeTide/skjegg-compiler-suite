/* mctest.c - a self-checking memory card test.
 *
 * Exercises the raw frame I/O and the save-file manager against the card the
 * emulator provides: a frame round-trip, then format, save (single block and a
 * multi-block save larger than 8 KB), list, load-back-and-compare, delete, and
 * confirm the deleted save is gone. Distinct nonzero exit codes name the step
 * that failed; 0 means the whole path works.
 */

#include "psx.h"

#define BIG 10000               /* > 8 KB, so it spans two blocks */

static unsigned char big_w[BIG];
static unsigned char big_r[BIG];
static char names[16][PSX_MC_NAME_MAX + 1];

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();

    if (!psx_mc_present())
        return 10;

    /* Raw frame round-trip on a scratch frame. */
    unsigned char w[PSX_MC_FRAME_SIZE], r[PSX_MC_FRAME_SIZE];
    for (int i = 0; i < PSX_MC_FRAME_SIZE; i++)
        w[i] = (unsigned char)(i * 7 + 3);
    if (psx_mc_write_frame(0x3F, w) != 0)
        return 11;
    if (psx_mc_read_frame(0x3F, r) != 0)
        return 12;
    for (int i = 0; i < PSX_MC_FRAME_SIZE; i++)
        if (r[i] != w[i])
            return 13;

    /* Format, then save a small file. */
    if (psx_mc_format() != 0)
        return 20;

    const char *msg = "hello save data";
    if (psx_mc_save("GAME-A", msg, 16) != 0)
        return 21;

    char back[32];
    int n = psx_mc_load("GAME-A", back, sizeof back);
    if (n != 16)
        return 22;
    for (int i = 0; i < 16; i++)
        if (back[i] != msg[i])
            return 23;

    /* A multi-block save (> 8 KB). */
    for (int i = 0; i < BIG; i++)
        big_w[i] = (unsigned char)(i * 131 + i / 251);
    if (psx_mc_save("GAME-B", big_w, BIG) != 0)
        return 30;
    int m = psx_mc_load("GAME-B", big_r, BIG);
    if (m != BIG)
        return 31;
    for (int i = 0; i < BIG; i++)
        if (big_r[i] != big_w[i])
            return 32;

    /* Both saves should be listed. */
    int count = psx_mc_list(names, 16);
    if (count != 2)
        return 40;

    /* Delete one; the other survives, the deleted one is gone. */
    if (psx_mc_delete("GAME-A") != 0)
        return 50;
    if (psx_mc_load("GAME-A", back, sizeof back) >= 0)
        return 51;
    if (psx_mc_load("GAME-B", big_r, BIG) != BIG)
        return 52;
    if (psx_mc_list(names, 16) != 1)
        return 53;

    return 0;
}
