/* The skj-cc-rv-psabi half of the stock-gcc interlink test: it provides the
 * global and the callee the stock gcc object reaches (through the GOT and a
 * PLT call), and calls into that object.  40 + 2 = 42. */

extern int gcc_compute(void);

int stock_base = 40;

int
stock_add(int a, int b)
{
    return a + b;
}

int
main(void)
{
    return gcc_compute();
}
