int main(void)
{
    int a, b, c;
    a = (b = 10, c = 20, b + c);
    return a;
}
