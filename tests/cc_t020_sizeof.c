/* cc_t020_sizeof.c : sizeof, checked with data-model-invariant relations so
   the test passes under both ILP32 and LP64. */

int main(void)
{
    int r = 0;
    if (sizeof(char) == 1) r += 1;
    if (sizeof(int) == 4) r += 2;
    if (sizeof(int *) == sizeof(long)) r += 4;   /* pointer == long, both models */
    int arr[10];
    if (sizeof(arr) == 10 * sizeof(int)) r += 8;
    return r;
}
