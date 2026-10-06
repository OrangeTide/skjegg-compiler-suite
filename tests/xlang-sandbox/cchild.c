/* The C child of the cross-language sandbox demo.
 *
 * Compiled by skj-cc-rv over the skj RV32 crt (start_rv.S supplies write/exit).
 * It sums 1..99 and prints the result, the same computation the Kobold child
 * does in Pascal, so the two markers line up and make the point: two source
 * languages, one IR, one backend, one runtime, side by side under one sandbox.
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

/* print an unsigned decimal with no runtime libc */
static void
emit_u(unsigned v)
{
    char buf[12];
    int n = 0;

    if (v == 0)
        buf[n++] = '0';
    while (v) {
        buf[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    char out[12];
    int j = 0;
    while (n)
        out[j++] = buf[--n];
    write(1, out, j);
}

int
main(void)
{
    unsigned sum = 0;
    unsigned i;

    emit("child[C]: running\n");
    for (i = 1; i <= 99; i++)
        sum += i;
    emit("child[C]: sum=");
    emit_u(sum);                /* 4950 */
    emit("\n");
    emit("child[C]: done\n");
    return 0;
}
