int main(void)
{
    int a[5];
    int i;
    for (i = 0; i < 5; i++)
        a[i] = i * i;
    return a[3] + a[4];
}
