/* C11 _Generic: selection by the controlling expression's static type.
   Covers scalar types, long long, pointer types by pointee, the default
   association, the non-evaluation of the controlling expression (the 1/0
   never traps), a 64-bit result keeping its width, and selection on an
   arithmetic expression's promoted type. All facts are target-independent;
   the program returns 42. */
#define tn(x) _Generic((x), \
    int: 1, \
    long long: 2, \
    double: 3, \
    char *: 4, \
    int *: 5, \
    default: 0)

int
main(void)
{
    int i = 0;
    long long ll = 0;
    double d = 0;
    char *p = 0;
    int *ip = 0;
    short s = 0;

    int acc = 0;
    acc += tn(i);          /* 1  */
    acc += tn(ll);         /* 2  */
    acc += tn(d);          /* 3  */
    acc += tn(p);          /* 4  */
    acc += tn(ip);         /* 5  */
    acc += tn(s);          /* 0 (default) */
    /* 1+2+3+4+5+0 = 15 */

    /* the controlling expression is not evaluated: a divide by zero here must
       not trap, because _Generic only inspects its type */
    acc += _Generic(1 / 0, int: 10, default: 99);  /* +10 = 25 */

    /* result keeps its 64-bit width in a wider computation */
    long long big = _Generic(i, int: 0x100000000LL, default: 0);
    acc += (int)(big >> 32);   /* +1 = 26 */

    /* arithmetic promotes the operand type: i + i is int */
    acc += _Generic(i + i, int: 16, long long: 0, default: 0);  /* +16 = 42 */

    return acc;
}