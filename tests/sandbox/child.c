/* sandbox demo: the spawned child.
 *
 * It prints a marker, maps a large region and touches every page, then prints
 * a second marker and exits. Touching the region is what draws on the aggregate
 * memory budget: under a generous --total-mem it finishes ("child done"); under
 * a tight one the pool is exhausted mid-loop and the child faults, while the
 * parent, isolated from it, runs on.
 */

extern int write(int fd, const char *buf, int n);
extern void *sys_map(unsigned size, unsigned flags);

#define CHILD_MAP   (16u * 1024u * 1024u)

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
    volatile unsigned char *p;
    unsigned off;

    emit("child running\n");

    p = sys_map(CHILD_MAP, 0);
    if (!p) {
        emit("child map failed\n");
        return 4;
    }

    for (off = 0; off < CHILD_MAP; off += 4096u)
        p[off] = 1;             /* commit each page; the pool caps this */

    emit("child done\n");
    return 0;
}
