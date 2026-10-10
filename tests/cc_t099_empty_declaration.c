/* A block-scope declaration with no declarator names no object: a bare type
   specifier (`int;`, `unsigned;`), a useless `extern int;`, or a struct tag
   declaration `struct S { int x; };`.  These declare nothing (the tag, if any,
   is registered during parsing), so the compiler must treat them as no-ops
   rather than crash on the absent name.  A nameless `extern int;` segfaulted
   the lowering before.  Self-checking, returns 42. */

int
main(void)
{
    int;                        /* a bare type specifier: declares nothing */
    unsigned;                   /* same */
    extern int;                 /* a useless storage class on an empty decl */

    struct pt { int x; int y; };   /* a tag declaration: no object named */
    struct pt p = { 40, 2 };       /* the tag is usable afterward */

    extern int;                 /* again, mid-block */

    return p.x + p.y;
}
