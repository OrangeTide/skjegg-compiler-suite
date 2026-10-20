/* bt_loop.c : host unit test for dw_backtrace's termination guards.  The walk
   reads a guest frame-pointer chain through a callback, so a corrupt or looping
   chain must be bounded by the host, never spin or run away.  This drives
   dw_backtrace with synthetic memory models and checks each guard:

     - an unbounded strictly-increasing chain stops at the depth cap;
     - a self-referential (cyclic) frame stops at the monotonic-fp check;
     - an unreadable/zero chain yields only the live pc;
     - a finite chain reports its real depth.

   No DWARF, no ELF, no qemu: it exercises the walk alone. */

#include "dwread.h"

#include <stdio.h>
#include <string.h>

#define MAXF    32
#define LIVE_PC 0x0000face

enum { M_CLIMB, M_CYCLE, M_UNMAPPED, M_FINITE };

static int g_mode;

/* The synthetic frame memory.  dw_backtrace reads prev-fp at [fp+0] and the
   return address at [fp+4], both words; model those two slots per mode. */
static int
tread(void *ctx, uint32_t addr, void *dst, uint32_t len)
{
    uint32_t v = 0;
    int is_prev = (addr & 0xf) == 0;    /* fp slots are 16-aligned here */

    (void)ctx;
    if (len != 4)
        return 0;
    switch (g_mode) {
    case M_CLIMB:                       /* prev always climbs, ra always set */
        v = is_prev ? addr + 16 : 0xABCD;
        break;
    case M_CYCLE:                       /* prev points back at this same fp */
        v = is_prev ? (addr & ~0xfu) : 0xABCD;
        break;
    case M_UNMAPPED:                    /* every slot reads as zero */
        v = 0;
        break;
    case M_FINITE:                      /* two real frames from base 0x3000 */
        if (addr == 0x3000) v = 0x3010; /* frame 0 prev -> frame 1 */
        else if (addr == 0x3004) v = 0xAA;
        else if (addr == 0x3010) v = 0; /* frame 1 prev = 0: chain ends */
        else if (addr == 0x3014) v = 0xBB;
        break;
    }
    memcpy(dst, &v, 4);
    return 1;
}

static int fails;

static void
expect(const char *what, int got, int want)
{
    if (got != want) {
        fprintf(stderr, "FAIL: %s: got %d, want %d\n", what, got, want);
        fails++;
    }
}

int
main(void)
{
    uint32_t pcs[MAXF];
    int n;

    g_mode = M_CLIMB;
    n = dw_backtrace(LIVE_PC, 0x1000, tread, NULL, pcs, MAXF);
    expect("unbounded climb is capped at the depth limit", n, MAXF);

    g_mode = M_CYCLE;
    n = dw_backtrace(LIVE_PC, 0x2000, tread, NULL, pcs, MAXF);
    expect("a cyclic frame stops at the monotonic-fp check", n, 2);

    g_mode = M_UNMAPPED;
    n = dw_backtrace(LIVE_PC, 0x9999, tread, NULL, pcs, MAXF);
    expect("an unreadable chain yields only the live pc", n, 1);

    g_mode = M_FINITE;
    n = dw_backtrace(LIVE_PC, 0x3000, tread, NULL, pcs, MAXF);
    expect("a finite chain reports its real depth", n, 3);

    /* the live pc is always frame 0, whatever the chain looks like */
    expect("frame 0 is the live pc", (int)(pcs[0] == LIVE_PC), 1);

    if (fails == 0)
        printf("PASS: dw_backtrace terminates on climb, cycle, unmapped, and finite chains\n");
    return fails ? 1 : 0;
}
