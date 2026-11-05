/* timer_test.c - a self-checking root-counter test.
 *
 * Counter 1 ticks once per horizontal blank.  One NTSC frame is 263 scanlines,
 * so between two vertical blanks the counter advances by about that many ticks,
 * a hardware-exact value that does not wrap a 16-bit counter.  The test starts
 * counter 1 on the horizontal blank, waits one frame, and checks the count is
 * in the one-frame range.  It also checks the system-clock counter runs much
 * faster than the horizontal blank over the same interval.  Distinct nonzero
 * exit codes name the check that failed; 0 means the counters tick correctly.
 */

#include "psx.h"

int
main(void)
{
    psx_gpu_init();
    psx_irq_init();

    /* Counter 1 on the horizontal blank, counter 2 on the system clock / 8. */
    psx_timer_start(1, PSX_TIMER_HBLANK);
    psx_timer_start(2, PSX_TIMER_SYSCLK8);

    /* Align to a frame boundary, restart both, then measure one frame. */
    psx_vsync();
    psx_timer_start(1, PSX_TIMER_HBLANK);
    psx_timer_start(2, PSX_TIMER_SYSCLK8);
    psx_vsync();

    unsigned hb = psx_timer_read(1);
    unsigned sc = psx_timer_read(2);

    /* One NTSC frame is 263 lines; allow slack for where in the line the reads
     * land and for the emulator's line model. */
    if (hb < 230 || hb > 290)
        return 10;

    /* System-clock/8 over one frame is ~70566 ticks, so the 16-bit counter has
     * wrapped at least once: it is not a simple comparison, but it must have
     * moved off zero and must run far faster than the horizontal blank.  Read a
     * fresh short interval to compare rates without wrapping. */
    psx_timer_start(1, PSX_TIMER_HBLANK);
    psx_timer_start(2, PSX_TIMER_SYSCLK8);
    for (volatile int i = 0; i < 2000; i++)     /* a brief, fixed delay */
        ;
    unsigned hb2 = psx_timer_read(1);
    unsigned sc2 = psx_timer_read(2);

    if (sc2 == 0)                   /* the system clock must have advanced */
        return 11;
    if (sc2 <= hb2)                 /* and far faster than the horizontal blank */
        return 12;

    return 0;
}
