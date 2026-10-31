/* spawn-kill demo: the child.  It would announce itself and exit cleanly, but
 * the parent kills it before it ever runs, so "child running" must not appear
 * in the output. */

extern int write(int fd, const char *buf, int n);

static void
emit(const char *s)
{
    int n = 0;

    while (s[n])
        n++;
    write(1, s, n);
}

int
main(void)
{
    emit("child running\n");
    return 0;
}
