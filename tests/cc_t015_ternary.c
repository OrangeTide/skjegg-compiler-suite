int main(void)
{
    int a = 10, b = 20;
    int max = (a > b) ? a : b;
    int min = (a < b) ? a : b;
    return max - min;
}
