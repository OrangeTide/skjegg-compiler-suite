int counter(void)
{
    static int n = 0;
    n++;
    return n;
}

int main(void)
{
    counter();
    counter();
    counter();
    return counter();
}
