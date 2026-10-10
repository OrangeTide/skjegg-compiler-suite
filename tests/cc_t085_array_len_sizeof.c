/* An implicit-dimension array's length (sizeof arr / sizeof arr[0]) must not
   depend on the element size.  Regression: an 8-byte element (a pointer on
   LP64, or a long long anywhere) made the inferred length too small, because
   the leaf-initializer count was divided by the element size in words. */

static const char *strs[] = { "a", "b", "c", "d", "e", "f",
                              "g", "h", "i", "j", "k" };    /* 11 */
static long long nums[] = { 1, 2, 3, 4, 5, 6, 7 };         /* 7 */
static int ints[] = { 10, 20, 30 };                        /* 3 */

int
main(void)
{
    if (sizeof strs / sizeof strs[0] != 11)
        return 1;
    if (sizeof nums / sizeof nums[0] != 7)
        return 2;
    if (sizeof ints / sizeof ints[0] != 3)
        return 3;
    return 42;
}
