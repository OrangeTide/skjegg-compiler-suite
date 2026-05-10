int main(void)
{
    int r = 0;
    if (sizeof(char) == 1) r += 1;
    if (sizeof(int) == 4) r += 2;
    if (sizeof(int *) == 4) r += 4;
    int arr[10];
    if (sizeof(arr) == 40) r += 8;
    return r;
}
