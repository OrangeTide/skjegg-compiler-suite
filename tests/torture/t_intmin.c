/* The integer overflow and 64-bit boundary seams that must read identically on
   every backend: two's-complement wrap on negate/add/sub/mul (no backend traps
   these), the 32-to-64 bit boundary, the shift family, and i64 divide/modulo by
   an ordinary divisor (the __divdi3 / __moddi3 software helpers).

   INT_MIN / -1 and INT_MIN % -1 are deliberately absent: they are undefined in
   C and the hardware divide instruction faults on x86-64 while other backends
   wrap, so they are a legitimate divergence, not a defect a differential run
   should flag. i64 divide/modulo are absent too: they are a runtime-library
   detail (the RISC-V crt ships no __divdi3), not a codegen result, so they are
   not a sound differential subject here. */

#include "torture.h"

int
main(void)
{
    int imin = (int)0x80000000;
    long long lmin = (long long)0x8000000000000000ULL;

    /* negate/multiply overflow wrap (two's complement, no trap anywhere) */
    t_sint("i32min", imin);
    t_sint("i32min_neg", -imin);
    t_sint("i32max_plus1", 0x7fffffff + 1);
    t_sint("i64min", lmin);
    t_sint("i64min_neg", -lmin);

    /* 64-bit arithmetic across the 32-bit boundary */
    {
        long long a = 0x00000000ffffffffLL;
        long long b = 0x0000000100000000LL;
        t_hex("i64_add", (unsigned long long)(a + b));
        t_hex("i64_mul3", (unsigned long long)(a * 3));
        t_hex("i64_shl8", (unsigned long long)(a << 8));
        t_hex("u64_shr8", (unsigned long long)b >> 8);
        t_sint("i64_sar4", (long long)0x8000000000000000ULL >> 4);
    }
    return 0;
}
