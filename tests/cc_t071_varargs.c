/* cc_t071_varargs.c : SysV varargs (x86-64 only; excluded on other targets) */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

typedef __builtin_va_list va_list;

int
isum(int n, ...)
{
    va_list ap;
    __builtin_va_start(ap, n);
    int s = 0, k;
    for (k = 0; k < n; k++)
        s = s + __builtin_va_arg_int(ap);
    __builtin_va_end(ap);
    return s;
}

double
dsum(int n, ...)
{
    va_list ap;
    __builtin_va_start(ap, n);
    double s = 0;
    int k;
    for (k = 0; k < n; k++)
        s = s + __builtin_va_arg_dbl(ap);
    __builtin_va_end(ap);
    return s;
}

long
lsum(int n, ...)
{
    va_list ap;
    __builtin_va_start(ap, n);
    long s = 0;
    int k;
    for (k = 0; k < n; k++)
        s = s + __builtin_va_arg_long(ap);
    __builtin_va_end(ap);
    return s;
}

int
main(void)
{
    int r = 0;

    /* 8 int args: 5 in registers, 3 overflow to the stack */
    if (isum(8, 1, 2, 3, 4, 5, 6, 7, 8) == 36) r += 1;

    /* float args in the SSE registers */
    if ((int)dsum(3, 1.5, 2.5, 3.0) == 7) r += 2;

    /* 64-bit long args */
    if (lsum(3, 100L, 200L, 300L) == 600) r += 4;

    return (r == 7) ? 42 : r;
}
