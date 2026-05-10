int double_it(int n)
{
    return n * 2;
}

int add(int a, int b)
{
    return a + b;
}

int main(void)
{
    return add(double_it(3), double_it(7));
}
