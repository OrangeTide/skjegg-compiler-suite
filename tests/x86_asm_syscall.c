/* x86-64 single-register-class constraints: the canonical Linux syscall idiom.
 * "=a"/"a" force rax, "D" rdi, "S" rsi, "d" rdx, so the operands land in the
 * registers the syscall ABI wants without local register variables.  The
 * output "=a"(ret) and the input "a"(1) share rax as a tied pair (the input is
 * moved in before the instruction, the output moved out after).  write(1, msg,
 * 22) returns the byte count, so the program exits 22 only if each constraint
 * placed its operand correctly. */

static long
sys_write(int fd, const char *buf, long n)
{
    long ret;

    asm volatile("syscall"
                 : "=a"(ret)
                 : "a"(1), "D"(fd), "S"(buf), "d"(n)
                 : "rcx", "r11", "memory");
    return ret;
}

int
main(void)
{
    const char *msg = "hello from pinned asm\n";    /* 22 bytes */

    return (int)sys_write(1, msg, 22);  /* 22 */
}
