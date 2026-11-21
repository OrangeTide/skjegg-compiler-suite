/* wasm_ld_d.c : the compute half of the skj-ld-wasm data link test.
   Sums n ints through the pointer wasm_ld_c.c passes in. */

int
combine(const int *p, int n)
{
    int s = 0, i;
    for (i = 0; i < n; i++)
        s += p[i];
    return s;
}
