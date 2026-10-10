/* Short-circuit logic, the ternary and comma operators, and the boolean value
   of comparisons. Side effects go through a global counter so a backend that
   wrongly evaluates a skipped operand is caught: the observed call count is the
   oracle, not just the final boolean. */

#include "torture.h"

static int calls;

static int
tick(int ret)
{
    calls++;
    return ret;
}

int
main(void)
{
    /* && short-circuits: the right operand is not evaluated when the left is 0 */
    calls = 0;
    int r1 = (tick(0) && tick(1));
    t_sint("and_val", r1);          /* 0 */
    t_sint("and_calls", calls);     /* 1: only the left ran */

    /* || short-circuits: the right operand is skipped when the left is nonzero */
    calls = 0;
    int r2 = (tick(5) || tick(1));
    t_sint("or_val", r2);           /* 1 (normalized to 0/1) */
    t_sint("or_calls", calls);      /* 1 */

    /* both operands run when the short circuit does not trigger */
    calls = 0;
    int r3 = (tick(1) && tick(7));
    t_sint("and2_val", r3);         /* 1 */
    t_sint("and2_calls", calls);    /* 2 */

    /* the ternary evaluates exactly one arm */
    calls = 0;
    int r4 = 1 ? tick(42) : tick(99);
    t_sint("tern_val", r4);         /* 42 */
    t_sint("tern_calls", calls);    /* 1 */

    /* comma evaluates left for effect, yields right */
    calls = 0;
    int r5 = (tick(3), tick(8));
    t_sint("comma_val", r5);        /* 8 */
    t_sint("comma_calls", calls);   /* 2 */

    /* logical not and comparison results are 0/1 */
    t_sint("not0", !0);             /* 1 */
    t_sint("not5", !5);             /* 0 */
    t_sint("cmp", (3 < 4) + (4 <= 4) + (5 > 9) + (2 != 2));  /* 1+1+0+0 = 2 */

    /* a chain of mixed logic with short circuit */
    calls = 0;
    int r6 = (tick(0) && tick(1)) || (tick(2) && tick(3));
    t_sint("chain_val", r6);        /* 1 */
    t_sint("chain_calls", calls);   /* tick(0),tick(2),tick(3) = 3 */

    return 0;
}
