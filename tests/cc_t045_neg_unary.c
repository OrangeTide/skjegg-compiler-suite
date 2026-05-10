int negate(int x)
{
    return -x;
}

int abs_val(int x)
{
    return x < 0 ? -x : x;
}

int main(void)
{
    int a = negate(10);
    int b = abs_val(a);
    int c = !0;
    int d = !5;
    return b + c - d;
}
