/* cc_t049_static_struct_array.c : static const array of structs with pointer members */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

struct entry {
    int code;
    const char *name;
    int len;
};

int lookup(int code)
{
    static const struct entry errors[] = {
        { 0,  "E_NONE",    6 },
        { 1,  "E_TYPE",    6 },
        { 2,  "E_INVARG",  8 },
        { 3,  "E_RANGE",   7 },
    };

    for (int i = 0; i < (int)(sizeof(errors) / sizeof(errors[0])); i++) {
        if (errors[i].code == code)
            return errors[i].len;
    }
    return 0;
}

int main(void)
{
    int r = 0;
    if (lookup(0) == 6)
        r += 1;
    if (lookup(3) == 7)
        r += 2;
    if (lookup(99) == 0)
        r += 4;
    return r;
}
