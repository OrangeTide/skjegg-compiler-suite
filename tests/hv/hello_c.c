/* hello_c_x86_64.c : a C guest for the x86-64 hardware-virtualization tier.
 * Compiled by skj-cc-x86-64, it reaches the host through the doorbell syscall
 * seam (write/exit), proving a native C program runs under KVM via skj-run. */

extern long write(int fd, const void *buf, long n);
extern void exit(int code);

static long
slen(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    return n;
}

int
main(int argc, char **argv)
{
    const char *msg = "hello from C under HV\n";

    (void)argc;
    (void)argv;
    write(1, msg, slen(msg));
    return 7;
}
