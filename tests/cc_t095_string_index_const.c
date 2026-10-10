/* A constant index of a string literal is a constant expression equal to the
   byte at that position: "abc"[1] is 'b', and index slen is the terminating
   NUL.  It errored as a non-constant initializer before.  Works in any integer
   constant context: a static char/int initializer and an array dimension.
   Self-checking, returns 42. */

static char b  = "abc"[1];      /* 'b' */
static char nl = "ab"[2];       /* the NUL terminator, 0 */
static int  z  = "xyz"[2];      /* 'z', into an int slot */
static int  e  = "abc"[1 + 1];  /* constant-expression index, 'c' */

char dim["hello"[0]];           /* array sized by a string index ('h' == 104) */

int
main(void)
{
    if (b != 'b')
        return 1;
    if (nl != 0)
        return 2;
    if (z != 'z')
        return 3;
    if (e != 'c')
        return 4;
    if (sizeof dim != (unsigned)'h')
        return 5;
    return 42;
}
