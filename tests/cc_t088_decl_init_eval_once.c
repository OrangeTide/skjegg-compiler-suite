/* A local declared with a function-call initializer must evaluate the call
   exactly once.  Before the fix, a second (or later) such declaration in a
   block was lowered twice: the ND_LOCAL_DECL case walked the declarator chain
   while the enclosing block already visited each flattened declaration, so a
   call initializer with a side effect ran an extra time.  A value-only test
   could not see it (re-evaluating a pure call gives the same answer); this one
   counts the calls.  Self-checking, returns 42. */

static int calls;

static int
bump(int v)
{
    calls++;
    return v;
}

int
main(void)
{
    /* Three declarations, each with a call initializer: three calls total. */
    int a = bump(10);
    int b = bump(20);
    int c = bump(30);

    if (a != 10 || b != 20 || c != 30)
        return 1;
    if (calls != 3)                     /* was 5 before the fix */
        return 2;

    /* A for-init with two call initializers must also run each once (the chain
       is not flattened into a block, so it is lowered in the for header). */
    calls = 0;
    {
        int s = 0, i, j;
        for (i = bump(0), j = bump(5); i < j; i++)
            s++;
        if (calls != 2)
            return 3;
        if (s != 5)
            return 4;
    }

    return 42;
}
