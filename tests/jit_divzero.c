/* jit_divzero.c : a runtime divide by zero.  The JIT guards the divisor
   (TRAP_GUARDED) and raises the DIV_ZERO fault, which the driver reports and
   exits 70 for.  Not part of the shared cc_* suite: the AOT tools use
   TRAP_HARDWARE, whose behavior on a zero divisor differs by target (AArch64
   returns 0, x86 faults), so this only makes sense run through a JIT. */

int
main(void)
{
    volatile int z = 0;         /* volatile so the divide is not folded away */
    volatile int x = 5;
    return x / z;
}
