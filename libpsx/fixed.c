/* fixed.c - libpsx 16.16 fixed-point math (milestone 4).
 *
 * The R3051 has no FPU, so PS1 games do their per-frame math in fixed point.
 * A fix16 is a 32-bit value with 16 fractional bits. Multiply uses the 64-bit
 * integer helpers the runtime already provides; divide is written with 32-bit
 * operations only, because the PS1 runtime has no 64-bit division helper.
 *
 * psx_sintab is a 1024-entry sine table (one full turn), generated at build
 * time by libpsx/tools/mktrig.c. Angles are in units of 1024 per turn.
 */

#include <stdint.h>

#include "psx.h"

extern const fix16 psx_sintab[1024];

fix16
psx_fmul(fix16 a, fix16 b)
{
    return (fix16)(((int64_t)a * (int64_t)b) >> 16);
}

fix16
psx_fdiv(fix16 a, fix16 b)
{
    int neg = 0;
    if (a < 0) {
        a = -a;
        neg ^= 1;
    }
    if (b < 0) {
        b = -b;
        neg ^= 1;
    }
    uint32_t na = (uint32_t)a;
    uint32_t nb = (uint32_t)b;
    if (nb == 0)
        return neg ? (fix16)0x80000000u : (fix16)0x7FFFFFFFu;

    /* Integer part with the native 32-bit divide, then 16 fractional bits by
       shift-and-subtract, so no 64-bit division helper is needed. */
    uint32_t q = na / nb;
    uint32_t rem = na % nb;
    uint32_t frac = 0;
    for (int i = 0; i < 16; i++) {
        rem <<= 1;
        frac <<= 1;
        if (rem >= nb) {
            rem -= nb;
            frac |= 1u;
        }
    }
    uint32_t r = (q << 16) | frac;
    return neg ? -(fix16)r : (fix16)r;
}

fix16
psx_fsin(int angle)
{
    return psx_sintab[(unsigned)angle & 1023u];
}

fix16
psx_fcos(int angle)
{
    return psx_sintab[((unsigned)angle + 256u) & 1023u];
}
