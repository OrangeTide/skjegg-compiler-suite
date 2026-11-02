/* driver_ret.c : a C program for the skjcc driver-integration link test on the
   64-bit hosted targets.  No libc (that tier links the crt and runtime helpers
   only), so it just computes and returns a value the crt passes to exit. */
int
main(void)
{
    int s = 0;
    for (int i = 1; i <= 9; i++)
        s += i;
    return s;           /* 45 */
}
