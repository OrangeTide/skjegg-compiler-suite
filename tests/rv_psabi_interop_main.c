/* The skj-cc-rv half of the interop test: compiled by the psABI RISC-V
 * C compiler, it calls a gcc-built function through the standard ILP32 ABI,
 * passing a 64-bit and a 32-bit argument and using the 64-bit result.
 * 20 * 2 + 2 = 42. */

extern long long gcc_scale(long long x, int k);

int
main(void)
{
    long long r = gcc_scale(20LL, 2);
    return (int)r;
}
