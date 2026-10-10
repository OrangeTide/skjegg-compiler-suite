/* Phase E libc self-check: the floating conversions f/e/g in the printf
   engine (libc/printf.c fmt_float).  Each case formats with snprintf and
   compares against the exact expected text.  Values stay within the double's
   ~15 significant digit budget, so the result is identical on a hardware FPU
   and the soft-float targets.  Returns 42 when every case matches. */

#include <stdio.h>
#include <string.h>

static int fails;

static void
chk(const char *want, const char *fmt, double v)
{
    char buf[64];
    snprintf(buf, sizeof buf, fmt, v);
    if (strcmp(buf, want) != 0)
        fails++;
}

int
main(void)
{
    /* %f */
    chk("3.141590", "%f", 3.14159);
    chk("0.000000", "%f", 0.0);
    chk("-0.000000", "%f", -0.0);
    chk("-2.500000", "%f", -2.5);
    chk("3.14", "%.2f", 3.14159);
    chk("4", "%.0f", 3.7);
    chk("       3.142", "%12.3f", 3.14159);
    chk("3.142       ", "%-12.3f", 3.14159);
    chk("+3.142", "%+.3f", 3.14159);
    chk("0003.142", "%08.3f", 3.14159);
    chk("100.00000", "%.5f", 100.0);
    chk("1234567.891000", "%f", 1234567.891);

    /* %e / %E */
    chk("3.141590e+00", "%e", 3.14159);
    chk("0.000000e+00", "%e", 0.0);
    chk("3.14e+04", "%.2e", 31415.9);
    chk("1.2345E-04", "%.4E", 0.00012345);
    chk("1e+01", "%.0e", 9.6);

    /* %g / %G */
    chk("3.14159", "%g", 3.14159);
    chk("100000", "%g", 100000.0);
    chk("1e+06", "%g", 1000000.0);
    chk("0.0001", "%g", 0.0001);
    chk("1e-05", "%g", 0.00001);
    chk("0", "%g", 0.0);
    chk("3.14", "%.3g", 3.14159);
    chk("1.50000", "%#g", 1.5);
    chk("0.00012345", "%G", 0.00012345);
    chk("-1e+01", "%.1g", -9.74);

    return fails == 0 ? 42 : 1;
}
