/* irq.c - libpsx interrupt-driven VBLANK (milestone 1).
 *
 * The C half of the VBLANK interrupt path. The low-level exception handler and
 * the privileged COP0 setup are in exc.S, because skj-as-mips encodes only the
 * instructions the backend emits and the backend never emits COP0 (mfc0, mtc0,
 * rfe); exc.S emits those as their raw encodings. This file enables the VBLANK
 * interrupt source and turns the handler's frame counter into a blocking wait.
 *
 * The interrupt controller registers are in the uncached KSEG1 window. I_MASK
 * gates which sources raise the CPU interrupt line; bit 0 is VBLANK (IRQ0),
 * which the display hardware raises every vertical blank.
 */

#include "psx.h"

#define I_MASK (*(volatile uint32_t *)0xBF801074u)
#define IRQ_VBLANK 0x0001u

/* Defined in exc.S: the counter the handler advances each VBLANK, and the
 * installer that hooks the vector, flushes the cache, and enables interrupts
 * in the CPU. */
extern volatile uint32_t __psx_vbl_count;
void __psx_irq_install(void);

void
psx_irq_init(void)
{
    I_MASK = I_MASK | IRQ_VBLANK;   /* unmask VBLANK before enabling the CPU line */
    __psx_irq_install();
}

uint32_t
psx_vblank_count(void)
{
    return __psx_vbl_count;
}

void
psx_vsync(void)
{
    uint32_t start = __psx_vbl_count;
    while (__psx_vbl_count == start)    /* sleep until the handler advances it */
        ;
}
