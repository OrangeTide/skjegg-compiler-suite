/* A C shim for the BASIC EXTERNAL i32-boundary test. Each function takes and
   returns a plain C `int` (32-bit). A BASIC program declares them EXTERNAL and
   calls them, so the import narrow (a 64-bit BASIC INT down to a C `int`
   argument) and the widen (a C `int` result sign-extended back to INT) are
   exercised end to end on a real target, where the register/stack width is what
   libpsx's own entry points expect. */

int
ext_add(int a, int b)
{
    return a + b;
}

int
ext_neg(int x)
{
    return -x;
}

int
ext_pick(int a, int b, int c, int d)
{
    return a * 1000 + b * 100 + c * 10 + d;
}
