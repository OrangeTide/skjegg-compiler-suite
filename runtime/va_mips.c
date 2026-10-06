/* va_mips.c - MIPS o32 va_arg helper for skj-cc.

   o32 lays every argument in one "argument structure": the first 16 bytes are
   the a0-a3 register home (a variadic prologue fills it) and the rest follow
   on the caller's stack, so the varargs are contiguous.  A two-word argument
   (long long or double) is 8-aligned in that structure.  The backend's
   IR_VA_START points overflow_arg_area at the first unnamed argument; cc
   lowers va_arg to a call to __va_arg, which 8-aligns the cursor for a 64-bit
   slot, returns a pointer to it, and advances by the slot size.  Compiled by
   the o32 gcc whose ABI the backend matches. */

struct __va_list_tag {
    unsigned gp_offset;
    unsigned fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
};

/* kind: 0 = 32-bit int/pointer (4 bytes), 1 = double, 2 = 64-bit integer */
void *
__va_arg(struct __va_list_tag *ap, int kind)
{
    char *p = ap->overflow_arg_area;
    void *r;

    if (kind != 0)              /* a two-word argument is 8-aligned in o32 */
        p = (char *)(((unsigned long)p + 7u) & ~(unsigned long)7u);
    r = p;
    p += (kind == 0) ? 4 : 8;
    ap->overflow_arg_area = p;
    return r;
}
