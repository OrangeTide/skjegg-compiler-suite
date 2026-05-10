int main(void)
{
    int x = 300;
    char c = (char)x;
    int y = (int)c;
    int *p = &x;
    int addr = (int)p;
    int *q = (int *)addr;
    return *q - x + 44;
}
