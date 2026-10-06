/* va_cf.c - ColdFire/m68k va_arg helper for skj-cc.

   The toolkit calling convention pushes every argument on the caller's stack
   (caller pops), so there is no register save area: the varargs sit
   contiguously just past the named parameters.  The backend's IR_VA_START
   points overflow_arg_area at the first unnamed argument; cc lowers va_arg to
   a call to __va_arg, which returns a pointer to the next slot and advances
   the va_list by the slot size.  Stack slots are 4-byte granular (m68k), so a
   32-bit value occupies one slot and a 64-bit value (double or long long) two.
   Compiled by m68k-linux-gnu-gcc, whose m68k C ABI the toolkit matches. */

struct __va_list_tag {
    unsigned gp_offset;
    unsigned fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
};

/* kind: 0 = 32-bit int/pointer, 1 = double, 2 = 64-bit integer */
void *
__va_arg(struct __va_list_tag *ap, int kind)
{
    void *p = ap->overflow_arg_area;
    ap->overflow_arg_area = (char *)p + (kind == 0 ? 4 : 8);
    return p;
}
