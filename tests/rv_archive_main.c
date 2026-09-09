/* Compiled by skj-cc-rv-psabi; pulls member A (and transitively B) out of the
 * archive.  40 + 1 + 1 = 42. */
extern int arch_needed(int x);

int
main(void)
{
    return arch_needed(40);
}
