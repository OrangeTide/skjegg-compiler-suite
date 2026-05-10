int main(void)
{
    int r = 0;
    if (7 % 3 == 1) r += 1;
    if (10 / 3 == 3) r += 2;
    if (-1 < 0) r += 4;
    int x = -7;
    if (x / 2 == -3) r += 8;
    return r;
}
