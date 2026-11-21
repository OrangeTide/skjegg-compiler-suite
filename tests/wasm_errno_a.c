/* exercise an external data symbol address (&errno) resolved across objects.

   errno is defined in wasm_errno_b.c; here it is extern, so skj-cc-wasm emits
   an undefined DATA symbol for it and a MEMORY_ADDR relocation on every access.
   skj-ld-wasm binds the undefined symbol to the definition in the other object.
   The store and the loads all go through the symbol's linear-memory address. */

extern int errno;

int
bump(int n)
{
    errno = n;                  /* store through &errno */
    errno = errno + 1;          /* load and store through &errno */
    return errno;               /* load through &errno */
}
