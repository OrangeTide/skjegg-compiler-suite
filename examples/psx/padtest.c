/* padtest.c - a self-checking controller test.
 *
 * Enables analog mode and reads the controller the emulator presents. Headless
 * PCSX-Redux presents a digital pad with no stick input, so this verifies what
 * is observable there: the analog-enable sequence runs without wedging the
 * port, a controller answers (a valid id, not "no device"), the length-aware
 * read parses the pad's response, and the stick axes read centered. A real
 * DualShock with a thumb on a stick would report that stick's position through
 * the same path. Distinct nonzero codes name the failed check.
 */

#include "psx.h"

static int
centered(int v)
{
    return v >= 0x78 && v <= 0x88;      /* 0x80 +/- a little */
}

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();

    psx_pad_init_analog();

    for (int f = 0; f < 10; f++) {
        psx_pad_read();
        psx_vsync();
    }

    int id = psx_pad_id();
    if (id == 0xFF)                     /* no controller answered */
        return 10;
    if (id != 0x41 && id != 0x73)       /* not a pad we understand */
        return 11;

    /* No input, so every axis must read centered (a digital pad reports all
     * four centered unconditionally). */
    if (!centered(psx_pad_stick_lx())) return 20;
    if (!centered(psx_pad_stick_ly())) return 21;
    if (!centered(psx_pad_stick_rx())) return 22;
    if (!centered(psx_pad_stick_ry())) return 23;

    return 0;
}
