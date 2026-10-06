/* sizeof a string literal measures the array storage (len + 1 for the NUL),
   not a pointer.  A string literal is char[N+1], decaying to char* only in
   value contexts.  Self-checking: returns 42.  Regression for the front end
   once typing a literal as char*, which made sizeof measure the pointer. */

int
main(void)
{
    int r = 0;

    /* the empty literal is char[1] (just the NUL) */
    if (sizeof("") == 1) r += 1;

    /* "hello" is char[6]: five bytes plus the NUL */
    if (sizeof("hello") == 6) r += 2;

    /* an escape is one byte, so "\n\t" is char[3] */
    if (sizeof("\n\t") == 3) r += 4;

    /* it still decays to char* in a value context: assign, index, read */
    {
        const char *p = "AB";
        if (p[0] == 'A' && p[1] == 'B' && p[2] == 0) r += 8;
    }

    /* and indexes directly as an array value too */
    if ("xyz"[1] == 'y') r += 16;

    return (r == 31) ? 42 : r;
}
