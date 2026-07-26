/* va_aarch64.c - AAPCS64 va_arg helper for cc-arm64 (P4-P2 varargs).
   Made by a machine. PUBLIC DOMAIN (CC0-1.0)

   cc lowers va_arg to a call to __va_arg, which returns a pointer to the next
   argument slot and advances the va_list.  The AArch64 va_list has separate
   general (GP) and SIMD/FP (VR) register save areas plus an overflow stack:
   __gr_offs / __vr_offs are negative offsets from __gr_top / __vr_top that
   count up toward zero, at which point arguments come from __stack.  Compiled
   by gcc, so it follows the AAPCS64 ABI that cc-arm64 now emits. */

struct __va_list {
    void *__stack;
    void *__gr_top;
    void *__vr_top;
    int __gr_offs;
    int __vr_offs;
};

void *
__va_arg(struct __va_list *ap, int is_fp)
{
    void *p;

    if (is_fp) {
        if (ap->__vr_offs < 0) {
            p = (char *)ap->__vr_top + ap->__vr_offs;
            ap->__vr_offs += 16;        /* v-regs use a 16-byte stride */
        } else {
            p = ap->__stack;
            ap->__stack = (char *)p + 8;
        }
    } else {
        if (ap->__gr_offs < 0) {
            p = (char *)ap->__gr_top + ap->__gr_offs;
            ap->__gr_offs += 8;
        } else {
            p = ap->__stack;
            ap->__stack = (char *)p + 8;
        }
    }
    return p;
}
