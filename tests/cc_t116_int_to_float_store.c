/* cc_t116_int_to_float_store.c : an integer assigned through a non-local float
   lvalue must be converted to the float, not stored as a raw integer.  The
   local-variable assignment path always converted; the pointer, array-element,
   struct-field and static-variable paths did not, so they stored an integer bit
   pattern (a stale SSE register on x86, an invalid i32-into-f64 store on WASM).
   Each case below stores an int, reads it back, and checks the float value. */

struct s {
    double f;
};

static double g;

static int
feq(double a, double b)
{
    double d = a - b;
    if (d < 0)
        d = -d;
    return d < 0.001;
}

int
main(void)
{
    int result = 0;
    double v;
    double *p = &v;
    double arr[2];
    struct s st;
    int i = 7;

    *p = 5;                     /* int constant through a pointer */
    if (feq(v, 5.0))
        result += 1;

    *p = i;                     /* int variable through a pointer */
    if (feq(v, 7.0))
        result += 2;

    arr[0] = 3;                 /* int constant into an array element */
    arr[1] = i;                 /* int variable into an array element */
    if (feq(arr[0], 3.0) && feq(arr[1], 7.0))
        result += 4;

    st.f = 9;                   /* int constant into a struct field */
    if (feq(st.f, 9.0))
        result += 8;

    g = 11;                     /* int constant into a static float */
    if (feq(g, 11.0))
        result += 16;

    return result;              /* 31 when every path converts */
}
