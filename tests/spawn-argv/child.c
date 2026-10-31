/* spawn-argv demo: the child.
 *
 * It is spawned by the parent through SKJ_SYS_SPAWNV with an argv vector, and
 * echoes what it received so the test can confirm the arguments crossed the
 * spawn boundary and the crt delivered them to main(argc, argv).
 */

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
main(int argc, char **argv)
{
    char d[2];
    int i;

    d[1] = 0;
    emit("child argc=");
    d[0] = (char)('0' + (argc % 10));
    emit(d);
    emit("\n");
    for (i = 0; i < argc; i++) {
        emit("child argv[");
        d[0] = (char)('0' + (i % 10));
        emit(d);
        emit("]=");
        emit(argv[i]);
        emit("\n");
    }
    return 0;
}
