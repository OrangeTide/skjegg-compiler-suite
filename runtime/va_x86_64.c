/* va_x86_64.c - SysV AMD64 va_arg helper for cc-x86-64 (P2 varargs).
   Made by a machine. PUBLIC DOMAIN (CC0-1.0)

   cc lowers va_arg to a call to __va_arg, which returns a pointer to the next
   argument slot and advances the va_list.  The register save area holds the
   6 general-purpose registers (48 bytes) then 8 SSE registers; overflow
   arguments live on the caller's stack.  Compiled by gcc, so it follows the
   SysV ABI that cc-x86-64 now emits. */

struct __va_list_tag {
    unsigned gp_offset;
    unsigned fp_offset;
    void *overflow_arg_area;
    void *reg_save_area;
};

void *
__va_arg(struct __va_list_tag *ap, int is_fp)
{
    void *p;

    if (is_fp) {
        if (ap->fp_offset < 176) {
            p = (char *)ap->reg_save_area + ap->fp_offset;
            ap->fp_offset += 16;
        } else {
            p = ap->overflow_arg_area;
            ap->overflow_arg_area = (char *)p + 8;
        }
    } else {
        if (ap->gp_offset < 48) {
            p = (char *)ap->reg_save_area + ap->gp_offset;
            ap->gp_offset += 8;
        } else {
            p = ap->overflow_arg_area;
            ap->overflow_arg_area = (char *)p + 8;
        }
    }
    return p;
}
