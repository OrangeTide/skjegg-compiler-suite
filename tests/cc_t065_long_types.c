/* cc_t065_long_types.c : integer-literal typing and size_t/ptrdiff_t, checked
   with data-model-invariant relations so it holds under both ILP32 and LP64. */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

typedef unsigned long size_t;
typedef long ptrdiff_t;

int arr[100];

int
main(void)
{
    int r = 0;

    /* an integer literal is typed by its suffix */
    if (sizeof(1) == sizeof(int)) r += 1;
    if (sizeof(1L) == sizeof(long)) r += 2;
    if (sizeof(1LL) == sizeof(long long)) r += 4;
    if (sizeof(1LL) == 8) r += 8;                 /* long long is 8 in both models */

    /* size_t / ptrdiff_t are pointer-width in every model */
    if (sizeof(size_t) == sizeof(void *)) r += 16;
    if (sizeof(ptrdiff_t) == sizeof(void *)) r += 32;

    /* they carry the right values */
    size_t n = sizeof(arr);                       /* 400 */
    ptrdiff_t d = &arr[70] - &arr[10];            /* 60 */
    if (n == 400 && d == 60) r += 64;

    return (r == 127) ? 42 : (r & 63);
}
