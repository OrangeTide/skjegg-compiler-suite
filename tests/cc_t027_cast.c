int main(void)
{
    int x = 300;
    char c = (char)x;
    int y = (int)c;
    int *p = &x;
    long addr = (long)p;        /* a pointer round-trips through a pointer-width
                                   integer; (int) would lose the top half on LP64 */
    int *q = (int *)addr;
    return *q - x + 44;
}
