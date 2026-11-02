/* cc_t112_sizeof_static_init.c : sizeof of an expression operand inside a
   file-scope (static-storage) initializer must fold to the operand's real
   type, not default to int.  The parser does not type expressions, so the
   file-scope constant folder once sized `sizeof arr` as int (4); lowering now
   points const_eval at its type resolver, so a declared array, struct, or
   element folds to its true size.  Pointer width is left ABI-dependent (4 on
   ILP32, 8 on LP64), so this checks it only for being pointer-sized, not a
   fixed number.
*/

int ext_arr[10];
static int sta_arr[8];
char buf[32];
int *ptr;
struct big { int a[10]; };
struct big gb;

/* every initializer below is folded at compile time, in static storage */
static int s_arr    = sizeof(ext_arr);
static int s_sta    = sizeof(sta_arr);
static int s_elems  = sizeof(ext_arr) / sizeof(ext_arr[0]);
static int s_buf    = sizeof(buf);
static int s_struct = sizeof(gb);
static int s_ptr    = sizeof(ptr);
static int s_deref  = sizeof(*ptr);
static int s_elem0  = sizeof(ext_arr[0]);

int
main(void)
{
    if (s_arr    != 40) return 1;
    if (s_sta    != 32) return 2;
    if (s_elems  != 10) return 3;
    if (s_buf    != 32) return 4;
    if (s_struct != 40) return 5;
    if (s_ptr < 4 || s_ptr > 8) return 6;    /* 4 on ILP32, 8 on LP64 */
    if (s_deref  != 4) return 7;
    if (s_elem0  != 4) return 8;
    return 42;
}
