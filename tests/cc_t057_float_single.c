/* cc_t057_float_single.c : float vs double, sizeof, function calls */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int ftoi_f(float x)
{
    return (int)x;
}

int ftoi_d(double x)
{
    return (int)x;
}

float add_f(float a, float b)
{
    return a + b;
}

int main(void)
{
    int result = 0;

    /* sizeof checks */
    if (sizeof(float) == 4)
        result += 1;
    if (sizeof(double) == 8)
        result += 2;

    /* float literal with f suffix */
    float f = 3.5f;
    if (ftoi_f(f) == 3)
        result += 4;

    /* double literal */
    double d = 7.25;
    if (ftoi_d(d) == 7)
        result += 8;

    /* float addition */
    float g = add_f(1.5f, 2.5f);
    if (ftoi_f(g) == 4)
        result += 16;

    /* float negation */
    float neg = -f;
    if (ftoi_f(neg) == -3)
        result += 32;

    return result;
}
