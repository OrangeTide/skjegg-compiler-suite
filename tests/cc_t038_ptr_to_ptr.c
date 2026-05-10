int main(void)
{
    int x = 42;
    int *p = &x;
    int **pp = &p;
    **pp = 99;
    return x;
}
