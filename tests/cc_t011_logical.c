int main(void)
{
    int a = 5, b = 0, c = 3;
    int r = 0;
    if (a && c)
        r = r + 1;
    if (a && b)
        r = r + 10;
    if (b || c)
        r = r + 100;
    return r;
}
