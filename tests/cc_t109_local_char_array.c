/* A local char array initialized from a string literal.  The inferred-length
 * form (char m[] = "...") takes its size from the string plus the terminating
 * NUL, so sizeof is right and the bytes are copied into the slot.  The
 * explicit-length form zero-fills the tail (char s[8] = "ab") and truncates a
 * too-long string without a NUL (char t[3] = "abcd").  Returns 42. */
int
main(void)
{
    char m[] = "abcd";
    char s[8] = "ab";
    char t[3] = "abcd";
    int i, sum;

    if (sizeof m != 5)          /* 4 chars + NUL */
        return 1;
    if (m[0] != 'a' || m[3] != 'd' || m[4] != '\0')
        return 2;

    if (sizeof s != 8)
        return 3;
    if (s[0] != 'a' || s[1] != 'b')
        return 4;
    sum = 0;
    for (i = 2; i < 8; i++)
        sum += s[i];            /* tail must be all zero */
    if (sum != 0)
        return 5;

    if (sizeof t != 3)
        return 6;
    if (t[0] != 'a' || t[1] != 'b' || t[2] != 'c')
        return 7;               /* truncated, no NUL stored */

    return 42;
}
