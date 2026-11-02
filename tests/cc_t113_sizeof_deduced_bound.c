/* cc_t113_sizeof_deduced_bound.c : an array whose bound is deduced from a
   string-literal initializer must carry that bound in its type, so sizeof is
   the string length plus the NUL.  A static-storage array (file-scope, and a
   function-local static) once kept length -1 because the bound was written only
   to the emitted slot, not the type, so sizeof folded to garbage; only a
   non-static local was right.  An explicit bound and a brace initializer are
   checked too, to show the deduction fires only when the bound is absent.
*/

static char fs[]  = "hello";          /* deduced -> 6 */
char        ext[] = "hello";          /* deduced -> 6 (external linkage) */
static char exp[10] = "hi";           /* explicit -> 10, not overridden */
static char br[]  = {'a', 'b', 'c'};  /* brace deduced -> 3 */

int
main(void)
{
    static char ls[] = "hello";       /* static local, deduced -> 6 */
    char la[] = "hello";              /* auto local, deduced -> 6 */

    if (sizeof(fs)  != 6)  return 1;
    if (sizeof(ext) != 6)  return 2;
    if (sizeof(ls)  != 6)  return 3;
    if (sizeof(la)  != 6)  return 4;
    if (sizeof(exp) != 10) return 5;
    if (sizeof(br)  != 3)  return 6;

    /* the storage is real: the deduced array holds the string and its NUL */
    if (fs[0] != 'h' || fs[5] != 0) return 7;
    if (br[2] != 'c') return 8;

    return 42;
}
