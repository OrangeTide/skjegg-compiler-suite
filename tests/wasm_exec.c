/* wasm_exec.c : exercise the linker-synthesized WASI runtime.

   skj-ld-wasm (no --no-entry) synthesizes _start, the write seam over WASI
   fd_write, and the exit seam over proc_exit.  main writes a line through the
   write seam and returns an exit code, so check-wasm-exec can compare both the
   stdout and the exit status from a module the in-tree linker built alone. */

extern int write(int fd, const void *buf, int n);

static int
slen(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    return n;
}

int
main(void)
{
    const char *msg = "wasm runtime up\n";
    write(1, msg, slen(msg));
    return 7;
}
