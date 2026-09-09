/* Archive member A: referenced by main, and itself referencing member B, so
 * skj-ld-rv must pull B by the fixpoint after pulling A. */
extern int arch_chain(int x);

int
arch_needed(int x)
{
    return arch_chain(x) + 1;
}
