/* wasm_ld_a.c : one half of the skj-ld-wasm cross-object link test.

   run() calls helper(), which is defined in wasm_ld_b.c, so linking the two
   objects exercises cross-object symbol resolution and the function-index
   relocation.  check-wasm-ld links them and checks run() returns 42. */

extern int helper(int a, int b);

int
run(void)
{
    return helper(40, 2);
}
