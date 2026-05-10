int main(void)
{
    int sum = 0;
    int i, j;
    for (i = 1; i <= 4; i++)
        for (j = 1; j <= i; j++)
            sum++;
    return sum;
}
