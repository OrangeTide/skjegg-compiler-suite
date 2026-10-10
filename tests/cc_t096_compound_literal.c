/* C99 compound literals: an anonymous object of a named type, (type){init}.
   At file scope and under static the object has static storage; at block scope
   it is automatic.  An array decays to a pointer to the object, a struct is an
   rvalue (or its address via &), and a postfix .member or [index] applies to
   the object.  They did not parse at all before.  Self-checking, returns 42. */

struct point {
    int x;
    int y;
};

/* file-scope: static-storage anonymous objects */
int          *g_arr = (int[]){ 4, 8, 15 };          /* pointer to an anon array */
struct point  g_s   = (struct point){ 1, 2 };       /* same-type struct init */
struct point *g_ps  = &(struct point){ 3, 4 };      /* address of an anon struct */

static int
sum(int *a, int n)
{
    int s = 0;
    for (int i = 0; i < n; i++)
        s += a[i];
    return s;
}

int
main(void)
{
    /* block scope: automatic anonymous objects */
    int *p = (int[]){ 10, 11, 21 };
    int  scalar = (int){ 7 };

    if (p[0] + p[1] + p[2] != 42)
        return 1;
    if (scalar != 7)
        return 2;
    /* pass a compound-literal array to a function (it decays) */
    if (sum((int[]){ 20, 22 }, 2) != 42)
        return 3;
    /* a postfix member and index directly on a literal */
    if ((struct point){ 40, 2 }.x + (struct point){ 0, 2 }.y != 42)
        return 4;
    if ((int[]){ 40, 2 }[0] + (int[]){ 0, 2 }[1] != 42)
        return 5;
    /* a fresh object each time the block is reached */
    int t = 0;
    for (int i = 0; i < 3; i++) {
        int *q = (int[]){ 14 };
        t += q[0];
    }
    if (t != 42)
        return 6;
    /* the file-scope objects */
    if (g_arr[0] + g_arr[1] + g_arr[2] != 27)
        return 7;
    if (g_s.x + g_s.y != 3)
        return 8;
    if (g_ps->x + g_ps->y != 7)
        return 9;
    return 42;
}
