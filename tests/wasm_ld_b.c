/* wasm_ld_b.c : the other half of the skj-ld-wasm cross-object link test.
   Defines the helper() that wasm_ld_a.c's run() calls. */

int
helper(int a, int b)
{
    return a + b;
}
