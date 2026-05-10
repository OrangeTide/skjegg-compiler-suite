int main(void)
{
    int x = 77;
    void *vp = &x;
    int *ip = (int *)vp;
    return *ip;
}
