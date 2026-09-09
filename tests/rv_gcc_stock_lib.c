/* The stock-gcc half of the RISC-V interlink test.  Built with gcc's default
 * flags (PIE and linker relaxation on), so it reaches an external global
 * through the GOT (R_RISCV_GOT_HI20 + R_RISCV_PCREL_LO12_I) and an external
 * function through the PLT-call relocation (R_RISCV_CALL_PLT).  skj-ld-rv must
 * synthesize the GOT for the static link and treat CALL_PLT as a direct call. */

extern int stock_base;
extern int stock_add(int a, int b);

int
gcc_compute(void)
{
    return stock_add(stock_base, 2);
}
