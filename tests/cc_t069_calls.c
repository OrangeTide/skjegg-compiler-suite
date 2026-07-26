/* cc_t069_calls.c : many args (register overflow), mixed int/float, returns */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/* 8 int args: under SysV the first 6 go in registers, the last 2 on the stack */
int sum8(int a, int b, int c, int d, int e, int f, int g, int h)
{
    return a + b + c + d + e + f + g + h;
}

/* mixed int/float/pointer, and > 8 floats to overflow the SSE registers */
double mix(int n, double a, int m, double b, double c, double d,
           double e, double f, double g, double h, double i)
{
    return (double)(n + m) + a + b + c + d + e + f + g + h + i;
}

int addp(int *p, int k) { return *p + k; }

int
main(void)
{
    int r = 0;
    if (sum8(1, 2, 3, 4, 5, 6, 7, 8) == 36) r += 1;      /* stack overflow args */
    double m = mix(10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10);   /* (10+2)+(1+3..10)=65 */
    if ((int)m == 65) r += 2;
    int x = 40;
    if (addp(&x, 2) == 42) r += 4;                        /* pointer arg */
    return (r == 7) ? 42 : r;
}
