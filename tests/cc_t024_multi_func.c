int is_even(int n);
int is_odd(int n);

int is_even(int n)
{
    if (n == 0) return 1;
    return is_odd(n - 1);
}

int is_odd(int n)
{
    if (n == 0) return 0;
    return is_even(n - 1);
}

int main(void)
{
    int r = 0;
    if (is_even(10)) r += 1;
    if (is_odd(7)) r += 2;
    if (!is_even(3)) r += 4;
    return r;
}
