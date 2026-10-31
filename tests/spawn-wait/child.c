/* spawn-wait demo: the child.  It announces itself and exits with a known,
 * non-zero status the parent reads back through SKJ_SYS_WAIT. */

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
    return 7;
}
