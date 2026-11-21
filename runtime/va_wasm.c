/* va_wasm.c - WebAssembly va_arg helper for skj-cc.

   skjegg compiles a variadic call under the fixed-plus-buffer ABI: the caller
   stores the unnamed arguments contiguously into a buffer in its frame (each
   8-byte slot 8-aligned) and passes the buffer's address as a hidden trailing
   argument.  The backend's IR_VA_START points overflow_arg_area at that buffer;
   cc lowers va_arg to a call to __va_arg, which 8-aligns the cursor for a
   64-bit slot, returns a pointer to it, and advances by the slot size.

   This is self-consistent for skj -> skj.  It is not the standard WASM C ABI
   (whose varargs buffer layout and promotion rules differ), so a skj-compiled
   variadic function does not interoperate with a clang-compiled caller and vice
   versa. */

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

    if (kind != 0)                      /* a two-word argument is 8-aligned */
        p = (char *)(((unsigned long)p + 7u) & ~(unsigned long)7u);
    r = p;
    p += (kind == 0) ? 4 : 8;
    ap->overflow_arg_area = p;
    return r;
}
