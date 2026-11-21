/* defines the errno that wasm_errno_a.c references externally, plus the entry
   the test invokes.  The linker merges this object's data segment and resolves
   the other object's undefined errno symbol to the address assigned here. */

int errno;

extern int bump(int n);

int
run(void)
{
    return bump(41);            /* 41 -> 42 through the shared errno */
}
