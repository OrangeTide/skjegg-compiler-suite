/* A standalone self-checking program for the skjcc cross driver: no libc, just
   a computation whose result is the process exit code.  Returns 42. */
int
main(void)
{
    int s = 0;
    for (int i = 0; i <= 8; i++)
        s += i;                 /* 0+1+...+8 = 36 */
    return (s == 36) ? 42 : 1;
}
