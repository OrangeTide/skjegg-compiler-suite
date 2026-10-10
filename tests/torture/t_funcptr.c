/* Function pointers: a call through a pointer variable, a pointer passed as an
   argument and called by the callee, and dispatch through an array of function
   pointers. The indirect-call path is calling-convention sensitive, so every
   backend must route arguments and the return value identically. */

#include "torture.h"

static int add(int a, int b) { return a + b; }
static int sub(int a, int b) { return a - b; }
static int mul(int a, int b) { return a * b; }

/* takes a function pointer and applies it */
static int
apply(int (*f)(int, int), int x, int y)
{
    return f(x, y);
}

int
main(void)
{
    /* call through a pointer variable */
    int (*p)(int, int) = add;
    t_sint("pvar", p(20, 22));          /* 42 */
    p = sub;
    t_sint("reassign", p(50, 8));       /* 42 */

    /* a pointer passed to a higher-order function */
    t_sint("apply_add", apply(add, 15, 27));   /* 42 */
    t_sint("apply_mul", apply(mul, 6, 7));     /* 42 */

    /* dispatch through an array of function pointers */
    int (*ops[3])(int, int) = { add, sub, mul };
    int acc = 0;
    for (int i = 0; i < 3; i++)
        acc += ops[i](10, 3);
    /* (10+3) + (10-3) + (10*3) = 13 + 7 + 30 = 50 */
    t_sint("table", acc);

    /* the address-of form is the same as the bare name */
    int (*q)(int, int) = &add;
    t_sint("addrof", q(40, 2));         /* 42 */

    return 0;
}
