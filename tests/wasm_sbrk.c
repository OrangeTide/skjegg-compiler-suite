/* wasm_sbrk.c : exercise the linker-synthesized __libc_sbrk seam.

   skj-ld-wasm defines __libc_sbrk over a break global and memory.grow when a
   program references it undefined (as the in-tree malloc does).  This checks
   the break is stable, returns the old break, advances by the request, and that
   a large request grows linear memory and hands back writable space. */

extern void *__libc_sbrk(long incr);
extern int write(int fd, const void *buf, int n);

int
main(void)
{
    char *a, *b, *big;

    a = __libc_sbrk(0);
    if (__libc_sbrk(0) != a)
        return 1;                       /* a query does not move the break */
    b = __libc_sbrk(100);
    if (b != a)
        return 2;                       /* returns the old break */
    if (__libc_sbrk(0) != a + 100)
        return 3;                       /* advanced by the request */

    /* ask for far more than the slack to the page edge, forcing memory.grow */
    big = __libc_sbrk(300000);
    if (big == (char *)-1)
        return 4;
    big[299999] = 'Z';
    if (big[299999] != 'Z')
        return 5;                       /* the grown page is usable */

    write(1, "sbrk ok\n", 8);
    return 0;
}
