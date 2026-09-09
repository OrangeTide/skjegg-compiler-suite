/* Archive member that nothing references.  It calls an undefined symbol, so if
 * skj-ld-rv wrongly pulled an unreferenced member the link would fail; a clean
 * link proves the member was skipped. */
extern int arch_absent(void);

int
arch_unused(void)
{
    return arch_absent();
}
