/* cc_t048_sizeof_struct.c : sizeof on structs and array-count idiom */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

struct entry {
    int code;
    const char *name;
    int len;
};

int main(void)
{
    int r = 0;

    if (sizeof(struct entry) == 12)
        r += 1;

    struct entry tbl[3];
    if (sizeof(tbl) == 36)
        r += 2;

    int count = sizeof(tbl) / sizeof(tbl[0]);
    if (count == 3)
        r += 4;

    return r;
}
