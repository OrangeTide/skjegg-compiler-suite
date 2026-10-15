/* Pinned extended inline asm on ColdFire, pinning allocatable registers.  The
 * m68k Linux syscall convention puts the call number in %d0 and the arguments
 * in %d1..%d5, and %d2..%d7 are the allocatable registers, so write(2) must
 * pin %d2 and %d3.  The register allocator reserves any register an asm pins or
 * clobbers, so no temp lives there across the trap.  write(1, msg, 22) returns
 * the byte count, so the program exits 22 only if every operand reached its
 * register and the in-out %d0 round-tripped. */

static int
sys_write(int fd, const char *buf, int n)
{
    register int d0 __asm__("%d0") = 4;         /* __NR_write */
    register int d1 __asm__("%d1") = fd;
    register int d2 __asm__("%d2") = (int)buf;  /* allocatable: reserved */
    register int d3 __asm__("%d3") = n;         /* allocatable: reserved */

    asm volatile("trap #0"
                 : "+r"(d0)
                 : "r"(d1), "r"(d2), "r"(d3)
                 : "memory");
    return d0;
}

int
main(void)
{
    const char *msg = "hello from pinned asm\n";    /* 22 bytes */
    int w = sys_write(1, msg, 22);

    return w;   /* 22 */
}
