struct vec2 {
    int x;
    int y;
};

struct rect {
    struct vec2 pos;
    struct vec2 size;
};

int area(struct rect *r)
{
    return r->size.x * r->size.y;
}

int main(void)
{
    struct rect r;
    r.pos.x = 0;
    r.pos.y = 0;
    r.size.x = 7;
    r.size.y = 9;
    return area(&r);
}
