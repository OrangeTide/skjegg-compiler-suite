/* cc_t078_large_frame.c : a stack frame larger than a 12-bit signed immediate
   (> 2047 bytes) and a call with more than 16 arguments.  The big local array
   pushes frame-relative slot and spill accesses past the reach of the direct
   `op reg, off(base)` / `addi dst, base, imm` form, exercising the RV backend's
   large-offset lowering (and the assembler's immediate range check); the
   20-argument call exercises the raised per-call argument cap.  Runs on every
   cc target. */

static int
sum20(int a, int b, int c, int d, int e, int f, int g, int h, int i, int j,
      int k, int l, int m, int n, int o, int p, int q, int r, int s, int t)
{
    return a + b + c + d + e + f + g + h + i + j
         + k + l + m + n + o + p + q + r + s + t;
}

int
main(void)
{
    int buf[800];               /* 3200 bytes: the frame exceeds 2047 */
    int i, acc;

    for (i = 0; i < 800; i++)
        buf[i] = i;

    acc = 0;
    for (i = 0; i < 800; i++)
        acc += buf[i];          /* 0 + 1 + ... + 799 = 319600 */

    /* 1 + 2 + ... + 20 = 210 */
    if (sum20(1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
              11, 12, 13, 14, 15, 16, 17, 18, 19, 20) != 210)
        return 1;

    if (acc != 319600)
        return 2;

    return 42;
}
