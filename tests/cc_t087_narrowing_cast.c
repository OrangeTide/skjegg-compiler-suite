/* A narrowing integer cast in rvalue context must reduce the value to the
   target type's range, not only when it is stored to a narrow lvalue.  Before
   the fix, (unsigned char)(expr) and friends were a no-op in an expression, so
   a value with set high bits compared and computed wrong.  Self-checking,
   returns 42. */

int
main(void)
{
    int x = 256 + 70;                   /* 326 */
    unsigned char k = 188;
    int tag = 68;
    signed char sc;
    short sh;
    int sum, j;

    if ((unsigned char)x != 70)                 /* 326 & 0xff = 70 */
        return 1;
    if ((unsigned char)(tag + k) != 0)          /* 256 & 0xff = 0 */
        return 2;
    if ((unsigned short)0x12345 != 0x2345)
        return 3;

    sc = (signed char)200;                      /* 200 -> -56 */
    if (sc != -56)
        return 4;
    if ((signed char)(0x7f + 1) != -128)        /* 128 -> -128 */
        return 5;

    sh = (short)0x8000;                         /* -32768 */
    if (sh != -32768)
        return 6;
    if ((short)0xffff != -1)
        return 7;

    /* used in arithmetic, not only in a comparison */
    sum = 0;
    for (j = 0; j < 300; j++)
        sum += (unsigned char)j;                /* 0..255 then 0..43 */
    if (sum != 33586)                           /* 32640 + 946 */
        return 8;

    return 42;
}
