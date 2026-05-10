int main(void)
{
    int x = 2;
    int r = 0;
    switch (x) {
    case 1:
        r += 1;
    case 2:
        r += 10;
    case 3:
        r += 100;
        break;
    case 4:
        r += 1000;
    }
    return r;
}
