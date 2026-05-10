/* cc_t054_float_basic.c : basic float arithmetic and conversions */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

double add(double a, double b)
{
    return a + b;
}

double mul(double a, double b)
{
    return a * b;
}

int ftoi(double x)
{
    return (int)x;
}

double itof(int x)
{
    return (double)x;
}

int main(void)
{
    int result = 0;

    /* basic float literal and conversion */
    double x = 3.5;
    if (ftoi(x) == 3)
        result += 1;

    /* float addition */
    double y = add(1.5, 2.25);
    if (ftoi(y) == 3)
        result += 2;

    /* float multiplication */
    double z = mul(2.0, 3.5);
    if (ftoi(z) == 7)
        result += 4;

    /* int-to-float promotion in arithmetic */
    double w = x + 1;
    if (ftoi(w) == 4)
        result += 8;

    /* float negation */
    double neg = -x;
    if (ftoi(neg) == -3)
        result += 16;

    /* compound assignment */
    double acc = 10.0;
    acc += 5.0;
    if (ftoi(acc) == 15)
        result += 32;

    return result;
}
