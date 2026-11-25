/* Two functions, only one reached from main.  Built with -ffunction-sections
   each lands in its own .text.<fn>, so a --gc-sections link drops unused while
   keeping used.  main returns used(41) == 42, so the process exit code is 42. */

int
used(int x)
{
    return x + 1;
}

int
unused(int x)
{
    return x * 7;
}

int
main(void)
{
    return used(41);
}
