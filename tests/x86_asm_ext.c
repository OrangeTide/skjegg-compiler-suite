/* Extended inline asm on x86-64: generic "r"/"=r"/"+r" operands with %N
 * substitution (the register name takes the operand's width, eax vs rax; the
 * backend emits NASM/Intel syntax, so templates and names carry no % prefix),
 * and register pinning through GNU local register variables bound to the Linux
 * x86-64 syscall registers (rax = number, rdi/rsi/rdx = args, result in rax).
 * Those registers are not allocatable, so the pins are free.  write(1, msg,
 * 22) returns the byte count, so the program reaches 42 only if every operand
 * landed in its register and the in-out rax round-tripped. */

static long
sys_write(int fd, const char *buf, long len)
{
    register long rax __asm__("rax") = 1;       /* __NR_write */
    register long rdi __asm__("rdi") = fd;
    register long rsi __asm__("rsi") = (long)buf;
    register long rdx __asm__("rdx") = len;

    asm volatile("syscall"
                 : "+r"(rax)
                 : "r"(rdi), "r"(rsi), "r"(rdx)
                 : "rcx", "r11", "memory");
    return rax;
}

int
main(void)
{
    const char *msg = "hello from pinned asm\n";    /* 22 bytes */
    long w = sys_write(1, msg, 22);
    int a = 40, b = 2, r;

    if (w != 22)
        return 1;

    /* a generic 32-bit add in Intel syntax: %0 = %1; %0 += %2 */
    asm("mov %0, %1\n\tadd %0, %2" : "=r"(r) : "r"(a), "r"(b));
    if (r != 42)
        return 2;

    /* pin two inputs to allocatable registers: the allocator reserves them */
    {
        register int rbx __asm__("rbx") = 40;
        register int r12 __asm__("r12") = 2;
        int q;
        asm("mov %0, %1\n\tadd %0, %2" : "=r"(q) : "r"(rbx), "r"(r12));
        if (q != 42)
            return 3;
    }
    return r;   /* 42 */
}
