/* cc_t076_long_double.c : long double is a distinct type with the correct ABI
   size (16 bytes on the psABI targets), usable where no value is materialized
   (sizeof, a pointer).  Actually operating on a long double value is a compile
   error (no 80-bit/128-bit float in the backend); that is checked by hand, not
   here, since the cc runner has no compile-error fixture.
   Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

/* a prototype referencing long double must parse (headers rely on this) */
long double declared_but_unused(long double);

/* a struct may CONTAIN a long double field (layout/sizeof must work); only
   operating on the field's value is an error, checked by hand */
struct with_ld { char c; long double x; };

int
main(void)
{
    /* sizeof is a compile-time constant: no value is materialized.  The size
       is the target's ABI size (16 on x86-64/arm64/rv64, 12 on m68k), so the
       portable invariant is that long double is wider than double. */
    int sz = (int)sizeof(long double);
    long double *p = 0;             /* a pointer to long double is fine */
    int psz = (int)sizeof(p);
    int stsz = (int)sizeof(struct with_ld);   /* struct with a long double field */
    return (sz > (int)sizeof(double) && psz > 0 && stsz > sz) ? 42 : sz;
}
