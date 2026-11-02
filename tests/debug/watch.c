/* Drives a gdb write watchpoint and FP-register access under skj-run --gdb.
   A watchpoint on gcounter fires when main writes it; the FP registers are
   exercised by the stub's p/P handlers (set then read back $ft0). */

volatile int gcounter;

static double
square(double x)
{
    return x * x;
}

int
main(void)
{
    double r = square(3.0);

    gcounter = 1;           /* a write watchpoint on gcounter stops here */
    gcounter = 2;
    return (int)r + gcounter - 11;   /* 9 + 2 - 11 = 0 */
}
