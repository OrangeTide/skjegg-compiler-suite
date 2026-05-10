typedef int i32;
typedef struct { int x; int y; } Point;

i32 dist_sq(Point *a, Point *b)
{
    i32 dx = a->x - b->x;
    i32 dy = a->y - b->y;
    return dx * dx + dy * dy;
}

int main(void)
{
    Point p1;
    Point p2;
    p1.x = 0; p1.y = 0;
    p2.x = 3; p2.y = 4;
    return dist_sq(&p1, &p2);
}
