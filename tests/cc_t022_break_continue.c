int main(void)
{
    int sum = 0;
    int i;
    for (i = 0; i < 20; i++) {
        if (i % 2 == 0) continue;
        if (i > 10) break;
        sum += i;
    }
    return sum;
}
