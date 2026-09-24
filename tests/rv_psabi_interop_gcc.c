/* The gcc-built half of the skj-cc-rv interop test: a stock RISC-V
 * RV32 (ILP32) function that receives a 64-bit and a 32-bit argument in the
 * standard register convention and returns a 64-bit result.  skj-cc-rv
 * output must reach it through that same ABI. */

long long
gcc_scale(long long x, int k)
{
    return x * (long long)k + 2;
}
