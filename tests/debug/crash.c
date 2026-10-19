/* crash.c : a nested call chain that faults, for the skj-run backtrace test.
   level3 reads through a null pointer, so skj-run takes a memory fault with the
   frame-pointer chain intact, and the symbolized backtrace should name level3,
   level2, level1, and main in order. */

int
level3(int *p)
{
    return *p;                  /* p is null: a read fault */
}

int
level2(int x)
{
    return level3((int *)0) + x;
}

int
level1(int x)
{
    return level2(x + 1);
}

int
main(void)
{
    return level1(1);
}
