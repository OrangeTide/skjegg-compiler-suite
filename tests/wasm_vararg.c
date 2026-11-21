/* exercise variadic calls under the WASM fixed-plus-buffer ABI.

   A variadic function takes a hidden pointer to the caller's varargs buffer;
   va_start captures it and __va_arg walks it.  This TU defines __va_arg itself
   (cc pre-declares struct __va_list_tag and __builtin_va_list), so the complete
   module is self-contained.  main returns a value assembled through several
   variadic calls, checked as the process exit code. */

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start((ap), (last))
#define va_arg(ap, type)   __builtin_va_arg((ap), type)
#define va_end(ap)         __builtin_va_end((ap))

/* kind: 0 = 32-bit int/pointer (4 bytes), 1 = double, 2 = 64-bit integer */
void *
__va_arg(struct __va_list_tag *ap, int kind)
{
    char *p = ap->overflow_arg_area;
    void *r;

    if (kind != 0)                      /* a two-word argument is 8-aligned */
        p = (char *)(((unsigned long)p + 7u) & ~(unsigned long)7u);
    r = p;
    p += (kind == 0) ? 4 : 8;
    ap->overflow_arg_area = p;
    return r;
}

/* sum `count` int arguments */
static int
sum(int count, ...)
{
    va_list ap;
    int total = 0, k;

    va_start(ap, count);
    for (k = 0; k < count; k++)
        total += va_arg(ap, int);
    va_end(ap);
    return total;
}

/* mix widths: an int, a long long, and another int, summed as 64-bit */
static long long
mix(int a, ...)
{
    va_list ap;
    long long acc = a;

    va_start(ap, a);
    acc += va_arg(ap, int);             /* +4-byte slot */
    acc += va_arg(ap, long long);       /* +8-byte slot, 8-aligned */
    acc += va_arg(ap, int);
    va_end(ap);
    return acc;
}

/* average `count` doubles passed as unnamed arguments */
static double
favg(int count, ...)
{
    va_list ap;
    double total = 0;
    int k;

    va_start(ap, count);
    for (k = 0; k < count; k++)
        total += va_arg(ap, double);
    va_end(ap);
    return total / count;
}

int
main(void)
{
    int s = sum(4, 10, 11, 12, 3);      /* 36 */
    long long m = mix(1, 2, 3LL, 0);    /* 6 */
    /* float arguments: promoted to double by the default argument promotions,
       so the 8-byte double va_arg reads them (not a mis-sized 4-byte single) */
    double d = favg(2, 1.5f, 2.5f);     /* (1.5 + 2.5) / 2 = 2.0 */
    if (s != 36)
        return 1;
    if (m != 6)
        return 2;
    if (d != 2.0)
        return 3;
    return s + (int)m + (int)d - 2;     /* 36 + 6 + 2 - 2 = 42 */
}
