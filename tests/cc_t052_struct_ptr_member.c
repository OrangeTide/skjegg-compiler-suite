/* cc_t052_struct_ptr_member.c : struct with pointer member, alloc and access */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

struct str {
    int len;
    const char *data;
};

int str_eq(struct str *a, struct str *b)
{
    if (a->len != b->len)
        return 0;
    for (int i = 0; i < a->len; i++) {
        if (a->data[i] != b->data[i])
            return 0;
    }
    return 1;
}

int main(void)
{
    struct str a;
    a.len = 3;
    a.data = "abc";

    struct str b;
    b.len = 3;
    b.data = "abc";

    struct str c;
    c.len = 3;
    c.data = "abd";

    int r = 0;
    if (str_eq(&a, &b))
        r += 1;
    if (!str_eq(&a, &c))
        r += 2;
    if (a.data[0] == 'a')
        r += 4;
    return r;
}
