/* Pinned extended inline asm on MIPS: GNU local register variables bind the
 * operands to the physical registers the Linux o32 syscall convention needs
 * ($v0 = number, $a0..$a2 = args, result back in $v0).  This is the real use
 * case for register pinning.  write(1, msg, 22) returns the byte count, so the
 * program exits 22 only if every operand reached its pinned register and the
 * in-out $v0 round-tripped. */

static long
sys_write(int fd, const char *buf, long len)
{
    register long v0 __asm__("$v0") = 4004;     /* __NR_write (o32) */
    register long a0 __asm__("$a0") = fd;
    register long a1 __asm__("$a1") = (long)buf;
    register long a2 __asm__("$a2") = len;

    asm volatile("syscall"
                 : "+r"(v0)
                 : "r"(a0), "r"(a1), "r"(a2)
                 : "memory");
    return v0;
}

int
main(void)
{
    const char *msg = "hello from pinned asm\n";    /* 22 bytes without the NUL */
    long w = sys_write(1, msg, 22);

    return (int)w;      /* 22 */
}
