/* rope.c : runtime rope operations for MooScript (m68k target) */

struct moo_rope_seg {
    const char *ptr;
    int len;
};

struct moo_rope {
    int count;
    struct moo_rope_seg segs[];
};

extern void *__moo_arena_alloc(int size);

struct moo_rope *
__moo_str_concat(struct moo_rope *a, struct moo_rope *b)
{
    int total = a->count + b->count;
    struct moo_rope *r = __moo_arena_alloc(4 + total * 8);
    int j = 0;

    r->count = total;
    for (int i = 0; i < a->count; i++)
        r->segs[j++] = a->segs[i];
    for (int i = 0; i < b->count; i++)
        r->segs[j++] = b->segs[i];
    return r;
}

int
__moo_str_eq(struct moo_rope *a, struct moo_rope *b)
{
    int alen = 0, blen = 0;
    int ai = 0, ao = 0, bi = 0, bo = 0;

    for (int i = 0; i < a->count; i++)
        alen += a->segs[i].len;
    for (int i = 0; i < b->count; i++)
        blen += b->segs[i].len;
    if (alen != blen)
        return 0;

    while (ai < a->count && bi < b->count) {
        int na = a->segs[ai].len - ao;
        int nb = b->segs[bi].len - bo;
        int n = na < nb ? na : nb;
        for (int k = 0; k < n; k++) {
            if (a->segs[ai].ptr[ao + k] != b->segs[bi].ptr[bo + k])
                return 0;
        }
        ao += n;
        bo += n;
        if (ao >= a->segs[ai].len) {
            ai++;
            ao = 0;
        }
        if (bo >= b->segs[bi].len) {
            bi++;
            bo = 0;
        }
    }
    return 1;
}

int
__moo_str_len(struct moo_rope *s)
{
    int len = 0;

    for (int i = 0; i < s->count; i++)
        len += s->segs[i].len;
    return len;
}
