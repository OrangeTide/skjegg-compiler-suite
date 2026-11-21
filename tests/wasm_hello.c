/* wasm_hello.c : a minimal program that exercises the synthesized WASI runtime.

   It defines main and calls the libc write seam, so the backend emits the
   _start entry and maps write/read/exit onto WASI preview1.  Used by check-wasm
   to verify the binary module runs under wasmtime and prints the expected line.
   The extern declarations are the seam the backend synthesizes; no libc header
   is needed. */

extern long write(int fd, const void *buf, unsigned long n);

static unsigned long
slen(const char *s)
{
    unsigned long n = 0;
    while (s[n])
        n++;
    return n;
}

int
main(void)
{
    const char *s = "hello from wasm\n";
    write(1, s, slen(s));
    return 0;
}
