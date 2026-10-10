/* Address-constant pointer initializers: a symbol plus a constant byte offset
   (&a[k], a + k, "s" + k, &s.field, &a[k] - 1), and an integer-constant
   pointer ((int *)N).  Each resolves to a relocation carrying the byte addend;
   earlier the offset forms either errored or silently emitted a zero pointer.
   Self-checking, returns 42. */

static int a[5] = { 10, 11, 12, 13, 14 };

int        *p  = a + 2;         /* &a[2], *p == 12 */
int        *q  = &a[3];         /* *q == 13 */
int        *pm = &a[4] - 1;     /* a + 3, *pm == 13 */
int        *arr[] = { &a[1], a + 4 };  /* *arr[0]==11, *arr[1]==14 */

char       *s2 = "hello" + 2;   /* points at "llo" */

struct P {
    int x;
    int y;
    int z;
};

static struct P pp = { 1, 2, 3 };
int        *py = &pp.y;         /* *py == 2 */

int        *hw = (int *)4096;   /* integer-constant pointer, not dereferenced */
int        *nul = (int *)0;     /* null pointer via cast */

int
main(void)
{
    if (*p != 12)
        return 1;
    if (*q != 13)
        return 2;
    if (*pm != 13)
        return 3;
    if (*arr[0] != 11 || *arr[1] != 14)
        return 4;
    if (s2[0] != 'l' || s2[1] != 'l' || s2[2] != 'o' || s2[3] != '\0')
        return 5;
    if (*py != 2)
        return 7;
    if ((unsigned long)hw != 4096u)
        return 8;
    if (nul != 0)
        return 9;
    return 42;
}
