/* Extended inline asm on RISC-V: generic "r"/"=r"/"+r" operands with %N
 * substitution, and register pinning through GNU local register variables
 * bound to the Linux ecall registers (a7 = number, a0..a2 = args, result back
 * in a0).  a0..a7 are not allocatable, so the pins are free.  write(1, msg, 22)
 * returns the byte count, so the program exits 22 only if every operand
 * reached its register and the in-out a0 round-tripped.  Returns 42 overall. */

static long
sys_write(int fd, const char *buf, long len)
{
    register long a7 __asm__("a7") = 64;        /* __NR_write (rv32) */
    register long a0 __asm__("a0") = fd;
    register long a1 __asm__("a1") = (long)buf;
    register long a2 __asm__("a2") = len;

    asm volatile("ecall"
                 : "+r"(a0)
                 : "r"(a7), "r"(a1), "r"(a2)
                 : "memory");
    return a0;
}

int
main(void)
{
    const char *msg = "hello from pinned asm\n";    /* 22 bytes */
    long w = sys_write(1, msg, 22);
    int a = 40, b = 2, r;

    if (w != 22)
        return 1;

    /* a generic three-operand add, no pinning */
    asm("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    if (r != 42)
        return 2;

    /* pin two inputs to allocatable s registers: the allocator reserves them */
    {
        register int s1 __asm__("s1") = 40;
        register int s2 __asm__("s2") = 2;
        int q;
        asm("add %0, %1, %2" : "=r"(q) : "r"(s1), "r"(s2));
        if (q != 42)
            return 3;
    }
    return r;   /* 42 */
}
