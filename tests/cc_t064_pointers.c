/* cc_t064_pointers.c : pointer arithmetic, deref, struct pointer fields, and
   the pointer/long size invariant.  Written data-model-invariant so it holds
   under both ILP32 and LP64 (this exercises the address lowering that becomes
   64-bit under LP64). */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int arr[5] = { 10, 20, 30, 40, 50 };
struct S { char c; long l; int *p; };

int
main(void)
{
    int r = 0;

    /* a pointer is the same width as a long in every model */
    if (sizeof(void *) == sizeof(long)) r += 1;

    /* pointer arithmetic and deref */
    int *p = arr;
    if (*(p + 3) == 40) r += 2;
    p = p + 2;
    if (*p == 30) r += 4;
    if ((p - arr) == 2) r += 8;         /* pointer difference */

    /* a pointer field lands at an aligned offset and round-trips */
    struct S s;
    s.p = &arr[4];
    s.l = 7;
    if (*s.p == 50) r += 16;
    if (s.l == 7) r += 32;

    /* an int* array element, written and read back through the pointer */
    int x = 99, *q = &x;
    *q = *q + 1;
    if (x == 100) r += 64;

    return (r == 127) ? 42 : (r & 63);
}
