int add(int a, int b) { return a + b; }
int mul(int a, int b) { return a * b; }

int apply(int (*fn)(int, int), int x, int y)
{
    return fn(x, y);
}

int main(void)
{
    int r = apply(add, 10, 20);
    r += apply(mul, 3, 4);
    return r;
}
