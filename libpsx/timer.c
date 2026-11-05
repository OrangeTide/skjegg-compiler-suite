/* timer.c - libpsx root counters (the three hardware timers).
 *
 * The PlayStation has three 16-bit counters at 0x1F801100, each with a count,
 * a mode, and a target register 0x10 apart.  They give a program a clock that
 * does not depend on the frame rate: counter 2 off the system clock for
 * profiling and delays, counter 1 off the horizontal blank for scanline-exact
 * timing, counter 0 off the GPU dot clock.  Writing the mode register resets
 * the count to zero, so psx_timer_start both selects the source and restarts
 * the counter.
 *
 * The registers are in the uncached KSEG1 window, like the rest of the I/O.
 * This driver free-runs a counter (wrap at 0xFFFF, no interrupt); a caller that
 * wants a target interrupt can program the mode register itself.
 */

#include "psx.h"

#define T_BASE 0xBF801100u

static volatile uint32_t *
treg(int counter, uint32_t off)
{
    return (volatile uint32_t *)(T_BASE + (uint32_t)counter * 0x10u + off);
}

void
psx_timer_start(int counter, int source)
{
    /* Mode: no synchronization (bit 0 clear, so the counter free-runs), wrap at
     * 0xFFFF (bit 3 clear), no interrupt (bits 4-6 clear), clock source in bits
     * 8-9.  The write also clears the count. */
    *treg(counter, 4) = ((uint32_t)(source & 3) << 8);
}

uint16_t
psx_timer_read(int counter)
{
    return (uint16_t)(*treg(counter, 0) & 0xFFFFu);
}
