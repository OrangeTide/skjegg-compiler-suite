int x = 10;

int main(void)
{
    int x = 20;
    int r = x;
    {
        int x = 30;
        r += x;
    }
    r += x;
    return r;
}
