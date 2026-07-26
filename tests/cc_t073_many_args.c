/* cc_t073_many_args.c : many scalar arguments, exercising register overflow
   to the stack (SysV: 6 int / 8 sse; AAPCS64: 8 int / 8 fp) and the stack
   convention on the other targets.  Runs on every cc target.
   Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int
isum10(int a, int b, int c, int d, int e,
       int f, int g, int h, int i, int j)
{
    return a + b + c + d + e + f + g + h + i + j;
}

double
dsum10(double a, double b, double c, double d, double e,
       double f, double g, double h, double i, double j)
{
    return a + b + c + d + e + f + g + h + i + j;
}

/* interleaved int and float args: each class fills its own registers */
double
mix8(int a, double b, int c, double d, int e, double f, int g, double h)
{
    return (double)(a + c + e + g) + b + d + f + h;
}

int
main(void)
{
    if (isum10(1, 2, 3, 4, 5, 6, 7, 8, 9, 10) != 55)
        return 1;

    /* evenly spaced 1.0..5.5 step 0.5, ten terms: sum 32.5 */
    if ((int)dsum10(1.0, 1.5, 2.0, 2.5, 3.0,
                    3.5, 4.0, 4.5, 5.0, 5.5) != 32)
        return 2;

    /* ints: 1+3+5+7 = 16; floats: 2.5+4.5+6.5+8.5 = 22.0; total 38.0 */
    if ((int)mix8(1, 2.5, 3, 4.5, 5, 6.5, 7, 8.5) != 38)
        return 3;

    return 42;
}
