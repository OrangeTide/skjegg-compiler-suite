/* Multi-file link through skjcc: this half calls a function from the other. */
extern int driver_addend(void);
int
main(void)
{
    return driver_addend() + 2;         /* 40 + 2 = 42 */
}
