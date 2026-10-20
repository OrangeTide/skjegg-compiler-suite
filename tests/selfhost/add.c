/* A small, include-free C program compiled in-guest by skj-cc running under
   skj-run (the self-host test).  add(40, 2) == 42 is the program's exit code,
   so the whole pipeline is checked end to end: skj-cc reads this source and
   writes assembly entirely through the sandbox syscalls, then that assembly is
   assembled, linked, and run. */
int
add(int a, int b)
{
    return a + b;
}

int
main(void)
{
    return add(40, 2);
}
