/* C99 variable arguments through the real <stdarg.h> (va_list, va_start,
   va_arg, va_end).  ILP32-safe: it uses int, double, long long, and a pointer,
   never a bare `long` whose width differs between the models, so it runs on
   every target whose backend implements varargs. Self-checking: returns 42. */

#include <stdarg.h>

static int
isum(int n, ...)
{
    va_list ap;
    int s = 0, k;
    va_start(ap, n);
    for (k = 0; k < n; k++)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}

static long long
lsum(int n, ...)
{
    va_list ap;
    long long s = 0;
    int k;
    va_start(ap, n);
    for (k = 0; k < n; k++)
        s += va_arg(ap, long long);
    va_end(ap);
    return s;
}

/* sum the doubles, scaled by 10 and rounded, so the check is an exact int */
static int
dsum10(int n, ...)
{
    va_list ap;
    double s = 0;
    int k;
    va_start(ap, n);
    for (k = 0; k < n; k++)
        s += va_arg(ap, double);
    va_end(ap);
    return (int)(s * 10.0 + 0.5);
}

/* return the first character of the last of n string arguments */
static char
lastfirst(int n, ...)
{
    va_list ap;
    char *p = 0;
    int k;
    va_start(ap, n);
    for (k = 0; k < n; k++)
        p = va_arg(ap, char *);
    va_end(ap);
    return p[0];
}

/* a named int, then a double and an int vararg: exercises mixed kinds */
static int
mixed(int a, ...)
{
    va_list ap;
    double d;
    int b;
    va_start(ap, a);
    d = va_arg(ap, double);
    b = va_arg(ap, int);
    va_end(ap);
    return a + (int)d + b;
}

int
main(void)
{
    /* 8 int args: on a register ABI this overflows the register file onto the
       stack, so the walk crosses the save-area/overflow boundary */
    if (isum(8, 1, 2, 3, 4, 5, 6, 7, 8) != 36)
        return 1;

    /* 64-bit integer varargs */
    if (lsum(3, 1000000000000LL, 2000000000000LL, 3LL) != 3000000000003LL)
        return 2;

    /* double varargs (1.5 + 2.5 + 3.0 = 7.0 -> 70) */
    if (dsum10(3, 1.5, 2.5, 3.0) != 70)
        return 3;

    /* pointer varargs */
    if (lastfirst(2, "xx", "hello") != 'h')
        return 4;

    /* mixed: 10 + (int)2.5 + 7 = 19 */
    if (mixed(10, 2.5, 7) != 19)
        return 5;

    return 42;
}
