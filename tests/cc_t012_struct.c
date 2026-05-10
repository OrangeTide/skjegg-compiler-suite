struct point {
    int x;
    int y;
};

int main(void)
{
    struct point p;
    p.x = 10;
    p.y = 32;
    return p.x + p.y;
}
