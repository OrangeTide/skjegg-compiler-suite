/* Extended inline asm on AArch64: generic "r"/"=r"/"+r" operands with %N
 * substitution (the register name takes the operand's width, w0 vs x0), and
 * register pinning through GNU local register variables bound to the Linux
 * AArch64 syscall registers (x8 = number, x0/x1/x2 = args, result in x0).
 * Those registers are not allocatable, so the pins are free.  write(1, msg, 22)
 * returns the byte count, so the program reaches 42 only if every operand
 * landed in its register and the in-out x0 round-tripped. */

static long
sys_write(int fd, const char *buf, long len)
{
    register long x8 __asm__("x8") = 64;        /* __NR_write */
    register long x0 __asm__("x0") = fd;
    register long x1 __asm__("x1") = (long)buf;
    register long x2 __asm__("x2") = len;

    asm volatile("svc #0"
                 : "+r"(x0)
                 : "r"(x8), "r"(x1), "r"(x2)
                 : "memory");
    return x0;
}

int
main(void)
{
    const char *msg = "hello from pinned asm\n";    /* 22 bytes */
    long w = sys_write(1, msg, 22);
    int a = 40, b = 2, r;

    if (w != 22)
        return 1;

    /* a generic 32-bit add: %0/%1/%2 substitute to w-registers */
    asm("add %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
    if (r != 42)
        return 2;

    /* pin two inputs to allocatable x registers: the allocator reserves them */
    {
        register int x19 __asm__("x19") = 40;
        register int x20 __asm__("x20") = 2;
        int q;
        asm("add %0, %1, %2" : "=r"(q) : "r"(x19), "r"(x20));
        if (q != 42)
            return 3;
    }
    return r;   /* 42 */
}
