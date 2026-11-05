/* pad_probe.c - milestone-2 controller read regression check.
 *
 * Reads controller port 1 once and exits 0 when a digital pad answered with
 * nothing pressed, the idle state a headless run sees. A broken SIO read
 * returns a wrong id or garbage buttons and exits nonzero, so a PCSX-Redux run
 * that reports 0 functionally verifies the direct-SIO read path.
 */

#include "psx.h"

int
main(void)
{
    uint16_t buttons = psx_pad_read();
    if (psx_pad_id() == 0x41 && buttons == 0)
        return 0;           /* digital pad, idle: the read path works */
    return 1;
}
