/* Basic inline assembly: the asm statement passes its string through to the
 * emitted assembly verbatim.  "nop" is a valid mnemonic on every backend
 * (m68k/ColdFire, x86, AArch64, RISC-V, MIPS), so the same source assembles
 * and runs on all of them.  Both the plain and the volatile spellings, and
 * adjacent-string concatenation, are exercised. */
int
main(void)
{
    int sum = 0;
    int i;

    for (i = 1; i <= 10; i++) {
        asm("nop");
        sum += i;
        asm volatile("nop");
    }

    /* adjacent string literals concatenate into one asm body */
    asm volatile("nop\n\t"
                 "nop");

    return sum;
}
