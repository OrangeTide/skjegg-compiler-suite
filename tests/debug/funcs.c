/* funcs.c : three functions of differing sizes, one static, so the assembler's
   STT_FUNC / st_size emission has distinct symbols and non-trivial sizes to
   pin.  The Makefile target test-rv-stt-func assembles this with skj-as-rv and
   checks each function is a sized FUNC symbol, cross-checked against GNU as. */

int
add(int a, int b)
{
    return a + b;
}

static int
helper(int x)
{
    int t = x * 3;
    return t + x;
}

int
main(void)
{
    return add(helper(2), 1);
}
