/* wasm_ld_e.c : the function-pointer half of the skj-ld-wasm table test.

   dbl's address is taken two ways: held in a data dispatch table (tab, a
   TABLE_INDEX_I32 data relocation) and passed directly as an argument (a
   TABLE_INDEX_SLEB code relocation).  Both hand the pointer to apply() in
   wasm_ld_f.c, which calls it indirectly, so the table slot has to agree
   across objects.  go and go2 each return 42. */

extern int apply(int (*f)(int), int x);

static int
dbl(int x)
{
    return x + x;
}

static int (*const tab[1])(int) = { dbl };

int
go(void)
{
    return apply(tab[0], 21);
}

int
go2(void)
{
    return apply(dbl, 21);
}
