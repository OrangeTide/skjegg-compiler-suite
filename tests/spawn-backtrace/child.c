/* spawn-backtrace child: a spawned process that faults in a named function, so
 * skj-run's fault backtrace for it (printed as "[proc 2]") has a real chain to
 * symbolize.  Built with -g, its ELF carries DWARF; the point of the test is
 * that a spawned child, not just the root, is symbolized now that each
 * process's image is retained. */

extern int write(int fd, const char *buf, int n);

static void
crash(void)
{
    volatile int *p = (volatile int *)0;        /* a null write: a bad-memory fault */

    *p = 42;
}

int
main(void)
{
    write(1, "child running\n", 14);
    crash();
    return 0;
}
