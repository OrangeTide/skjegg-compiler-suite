/* Extended inline asm on ColdFire: generic "r"/"=r"/"+r" operands with %N
 * substitution, and register pinning through GNU local register variables
 * (register names are the assembler's own spelling, %d0/%d1).  Pins land only
 * on the non-allocatable scratch data registers; the m68k syscall registers
 * d1..d5 are allocatable, so a real trap stays in hand-written assembly.
 * Returns 42. */
int
main(void)
{
    int a = 40, b = 2, r;
    register int x __asm__("%d0") = 40;
    register int y __asm__("%d1") = 2;

    /* r = a + b  (write-only output, two generic inputs) */
    asm("move.l %1, %0\n\tadd.l %2, %0" : "=r"(r) : "r"(a), "r"(b));
    if (r != 42)
        return 1;

    /* x += y  with x and y pinned to %d0 and %d1 (in-out plus input) */
    asm("add.l %1, %0" : "+r"(x) : "r"(y));
    if (x != 42)
        return 2;

    /* register-class constraints: "d" (data) reads like "r"; "a" puts the
       operand in an address register for the indirect load */
    {
        int t[2];
        int q;
        t[1] = 42;
        asm("move.l (%1), %0" : "=d"(q) : "a"(&t[1]));
        if (q != 42)
            return 3;
    }

    return r;   /* 42 */
}
