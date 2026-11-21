/* wasm_ld_c.c : the data half of the skj-ld-wasm link test.

   vals is a data array and vp is a pointer into it held in data, so linking
   exercises the memory-address relocations: the pointer-in-data reloc that
   fixes vp to the final address of vals, and the code reloc that loads vp.
   sum3() hands the pointer to combine() in wasm_ld_d.c, so the data address
   also has to be correct across objects.  check-wasm-ld expects 42. */

extern int combine(const int *p, int n);

static const int vals[3] = { 10, 15, 17 };
static const int *const vp = vals;

int
sum3(void)
{
    return combine(vp, 3);
}
