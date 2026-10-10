/* The offsetof idiom in a static initializer: (unsigned)&((T *)0)->member is a
   constant equal to the member's byte offset (a constant-pointer base plus a
   field offset).  It errored as a non-constant initializer before.  Offsets
   are target-dependent, so each is checked against the same offset computed at
   run time rather than a hardcoded number.  Self-checking, returns 42. */

struct S {
    char   a;
    int    b;
    double c;
};

struct T {
    int x;
    int arr[4];
};

static unsigned ob = (unsigned)&((struct S *)0)->b;        /* offsetof(S, b) */
static unsigned oc = (unsigned)&((struct S *)0)->c;        /* offsetof(S, c) */
static unsigned oa = (unsigned)&((struct T *)0)->arr[2];   /* offsetof(T, arr[2]) */
int            *pb = &((struct S *)0)->b;                   /* same, pointer-typed */

int
main(void)
{
    struct S s;
    struct T t;
    char *sb = (char *)&s;
    char *tb = (char *)&t;

    if (ob != (unsigned)((char *)&s.b - sb))
        return 1;
    if (oc != (unsigned)((char *)&s.c - sb))
        return 2;
    if (oa != (unsigned)((char *)&t.arr[2] - tb))
        return 3;
    if ((unsigned long)pb != ob)
        return 4;
    return 42;
}
