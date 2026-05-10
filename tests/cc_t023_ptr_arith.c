int main(void)
{
    int arr[5];
    int *p = arr;
    int i;
    for (i = 0; i < 5; i++)
        *(p + i) = i * 10;
    int *q = p + 3;
    return *q + *(q - 1);
}
