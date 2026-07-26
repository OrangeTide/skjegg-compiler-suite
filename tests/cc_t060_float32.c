/* cc_t060_float32.c : native single-precision float and float<->double,
 * int<->float conversions (IR_F32).  All checks are integer-valued so the
 * result is stable on ColdFire's compute-wide FPU. */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

float add_f(float a, float b)
{
    return a + b;
}

double widen(float x)
{
    return x;                    /* float -> double */
}

float narrow(double x)
{
    return x;                    /* double -> float */
}

int main(void)
{
    int result = 0;

    /* int -> float */
    float h = 10;
    if ((int)h == 10)
        result += 1;

    /* float -> double via return, then arithmetic in double */
    float f = 3.5f;
    double d = widen(f);
    if ((int)(d * 2.0) == 7)     /* 3.5 * 2 = 7 */
        result += 2;

    /* double -> float via return */
    double e = 2.25;
    float g = narrow(e);
    if ((int)(g * 4.0f) == 9)    /* 2.25 * 4 = 9 */
        result += 4;

    /* mixed float + double -> double */
    double mix = f + e;          /* 3.5 + 2.25 = 5.75 */
    if ((int)mix == 5)
        result += 8;

    /* float + float -> float, passed and returned across a call */
    float s = add_f(1.5f, 2.5f); /* 4.0 */
    if ((int)s == 4)
        result += 16;

    /* explicit casts both directions round-trip an exact value */
    float rt = (float)(double)6.5f;
    if ((int)rt == 6)
        result += 32;

    return result;               /* expect 63 */
}
