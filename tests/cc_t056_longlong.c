/* cc_t056_longlong.c : long long arithmetic, casts, and function calls */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

long long add64(long long a, long long b)
{
    return a + b;
}

long long sub64(long long a, long long b)
{
    return a - b;
}

long long mul64(long long a, long long b)
{
    return a * b;
}

int trunc64(long long x)
{
    return (int)x;
}

long long widen(int x)
{
    return (long long)x;
}

int main(void)
{
    int result = 0;

    /* local variable, assignment, truncation to int */
    long long x = 100;
    if (trunc64(x) == 100)
        result += 1;

    /* addition */
    long long y = add64(30, 12);
    if (trunc64(y) == 42)
        result += 2;

    /* subtraction */
    long long z = sub64(100, 58);
    if (trunc64(z) == 42)
        result += 4;

    /* multiplication */
    long long m = mul64(6, 7);
    if (trunc64(m) == 42)
        result += 8;

    /* bitwise AND */
    long long a = 0xFF;
    long long b = 0x0F;
    if (trunc64(a & b) == 0x0F)
        result += 16;

    /* bitwise OR */
    if (trunc64(a | 0x100) == 0x1FF)
        result += 32;

    /* left shift */
    long long s = 1;
    s = s << 4;
    if (trunc64(s) == 16)
        result += 64;

    /* right shift */
    long long r = 256;
    r = r >> 3;
    if (trunc64(r) == 32)
        result += 128;

    return result;
}
