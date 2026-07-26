/* cc_t048_sizeof_struct.c : sizeof on structs and the array-count idiom,
   checked with data-model-invariant relations (an array is count * element,
   and the count idiom holds) so the test passes under both ILP32 and LP64. */
/* Made by a machine. PUBLIC DOMAIN (CC0-1.0) */

struct entry {
    int code;
    const char *name;
    int len;
};

int main(void)
{
    int r = 0;

    struct entry tbl[3];

    if (sizeof(tbl) == 3 * sizeof(struct entry))
        r += 1;

    if (sizeof(tbl[0]) == sizeof(struct entry))
        r += 2;

    int count = sizeof(tbl) / sizeof(tbl[0]);
    if (count == 3)
        r += 4;

    return r;
}
