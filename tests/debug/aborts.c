/* aborts.c : a nested call chain that fails an assert, for the skj-run
   abort/assert backtrace test.  assert in deep() fails, so __assert_fail prints
   its message and calls abort(), which asks skj-run (through the RV_SYS_FATAL
   ecall) for a symbolized backtrace before exiting 134.  The backtrace should
   name abort, __assert_fail, deep, mid, and main. */

#include <assert.h>

int
deep(int x)
{
    assert(x == 0);             /* x is 1: the assert fails */
    return x;
}

int
mid(int x)
{
    return deep(x) + 1;
}

int
main(void)
{
    return mid(1);
}
