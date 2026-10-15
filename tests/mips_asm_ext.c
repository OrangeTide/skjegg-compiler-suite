/* Extended inline asm on MIPS: "r"-class operand constraints with %N
 * substitution.  Exercises a write-only output ("=r"), plain inputs ("r"), an
 * in-out operand ("+r"), and a "memory" clobber.  The compiler places each
 * operand in a register and substitutes %0.. in the template.  Returns 42. */

int
main(void)
{
    int a = 40, b = 2, r;
    int c = 2;

    /* r = a + b  (write-only output, two inputs) */
    asm("addu %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));

    /* r -= c, then r += c  (in-out operand, read and written) */
    asm volatile("subu %0, %0, %1" : "+r"(r) : "r"(c) : "memory");
    asm volatile("addu %0, %0, %1" : "+r"(r) : "r"(c));

    /* pin two inputs to allocatable $s registers: the allocator reserves
       them, so no temp lives there across the asm */
    {
        register int s2 __asm__("$s2") = 40;
        register int s3 __asm__("$s3") = 2;
        int q;
        asm("addu %0, %1, %2" : "=r"(q) : "r"(s2), "r"(s3));
        if (q != 42)
            return 9;
    }

    return r;   /* 42 */
}
