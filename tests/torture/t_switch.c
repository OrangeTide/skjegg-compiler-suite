/* switch lowering: a dense run that a backend may turn into a jump table, a
   sparse set that stays a comparison chain, fallthrough, default, a switch on a
   negative and on a wide (long long) control value, and a nested switch. Every
   backend must pick the same arm, so the accumulated result is the oracle. */

#include "torture.h"

static int
dense(int x)
{
    int r = 0;
    switch (x) {
    case 0: r = 10; break;
    case 1: r = 11; break;
    case 2: r = 12; /* fall through */
    case 3: r += 100; break;
    case 4: r = 14; break;
    case 5: r = 15; break;
    case 6: r = 16; break;
    case 7: r = 17; break;
    default: r = -1; break;
    }
    return r;
}

static int
sparse(int x)
{
    switch (x) {
    case -5: return 1;
    case 1000: return 2;
    case 32767: return 3;
    case -32768: return 4;
    default: return 9;
    }
}

int
main(void)
{
    for (int i = -1; i <= 8; i++)
        t_sint("dense", dense(i));

    t_sint("sparse_a", sparse(-5));
    t_sint("sparse_b", sparse(1000));
    t_sint("sparse_c", sparse(32767));
    t_sint("sparse_d", sparse(-32768));
    t_sint("sparse_e", sparse(7));

    /* switch on a 64-bit control value, including a case constant that does
       not fit in 32 bits */
    {
        long long v = 0x100000003LL; /* low word 3, high word set */
        int r = 0;
        switch (v) {
        case 1: r = 1; break;
        case 0x100000003LL: r = 42; break;
        default: r = 0; break;
        }
        t_sint("ll_switch", r);

        long long w = 2;
        int q = 0;
        switch (w) {        /* a small value a 32-bit compare could alias */
        case 1: q = 1; break;
        case 2: q = 99; break;
        default: q = 0; break;
        }
        t_sint("ll_small", q);
    }

    /* nested switch with fallthrough reaching the inner default */
    {
        int acc = 0;
        for (int a = 0; a < 3; a++) {
            switch (a) {
            case 0:
            case 1:
                switch (a) {
                case 1: acc += 7; break;
                default: acc += 1; break;
                }
                break;
            default:
                acc += 100;
                break;
            }
        }
        t_sint("nested", acc);   /* a=0 ->1, a=1 ->7, a=2 ->100 = 108 */
    }

    return 0;
}
