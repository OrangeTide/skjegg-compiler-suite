/* cc_t053_nested_for.c : nested for with C99 decl and early break */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

int search(int *arr, int n, int target)
{
    for (int i = 0; i < n; i++) {
        if (arr[i] == target)
            return i;
    }
    return -1;
}

int main(void)
{
    int data[5] = {10, 20, 30, 40, 50};
    int r = 0;

    int idx = search(data, 5, 30);
    if (idx == 2)
        r += 1;

    idx = search(data, 5, 99);
    if (idx == -1)
        r += 2;

    int sum = 0;
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j <= i; j++)
            sum += 1;
    }
    if (sum == 15)
        r += 4;

    return r;
}
