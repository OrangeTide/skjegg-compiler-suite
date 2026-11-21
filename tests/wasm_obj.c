/* wasm_obj.c : a program for the relocatable-object path.

   It exercises the object format that check-wasm-obj links with wasm-ld:
   function-index relocations (the call chain), the stack-pointer global import
   (the recursive frame), a data symbol with memory-address relocations (reading
   the bonus array), a function pointer in data (a table-index data relocation
   and an indirect call through it), and the linking symbol table.  compute()
   returns 42; check-wasm-obj links the object and checks that under wasmtime. */

static const int bonus[3] = {1, 2, 3};

static int
addn(int a, int b)
{
    return a + b;
}

/* a function pointer in data: its initializer is a TABLE_INDEX_I32 relocation */
static int (*const ops[1])(int, int) = { addn };

static int
sum_to(int n)
{
    if (n <= 0)
        return 0;
    return addn(n, sum_to(n - 1));
}

int
compute(void)
{
    int b = bonus[0] + bonus[1] + bonus[2];     /* 6, read from data */
    return ops[0](sum_to(8), b);                /* 36 + 6 = 42, via a data fn ptr */
}
