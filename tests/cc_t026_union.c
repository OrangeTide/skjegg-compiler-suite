union val {
    int i;
    char c[4];
};

int main(void)
{
    union val v;
    v.i = 0;
    v.c[0] = 42;
    return v.c[0];
}
