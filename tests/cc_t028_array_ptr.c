int sum(int *arr, int n)
{
    int s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += arr[i];
    return s;
}

int main(void)
{
    int a[6];
    a[0] = 10; a[1] = 20; a[2] = 30;
    a[3] = 40; a[4] = 50; a[5] = 60;
    return sum(a, 6) - 100;
}
