/* wasm_ld_f.c : the indirect-call half of the skj-ld-wasm table test.
   Calls the function pointer wasm_ld_e.c hands it (a call_indirect). */

int
apply(int (*f)(int), int x)
{
    return f(x);
}
