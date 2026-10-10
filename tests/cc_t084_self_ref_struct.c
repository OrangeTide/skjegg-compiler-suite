/* cc_t084_self_ref_struct.c : a struct with a pointer to its own type, and
   chained access through that pointer (node->next->value, node->next->next).
   Pins that a self-referential field resolves to the completed struct, so the
   second arrow finds the pointee's fields rather than a fieldless forward stub.
   Self-checking: returns 42. */

struct node {
    struct node *next;
    int value;
};

int
main(void)
{
    struct node c = { 0, 3 };
    struct node b = { &c, 2 };
    struct node a = { &b, 1 };
    struct node *p = &a;
    int r = 0;

    /* one hop */
    if (p->next->value == 2)
        r += 1;
    /* two hops, through two pointer fields */
    if (p->next->next->value == 3)
        r += 2;
    /* the end of the list is reached through the chain */
    if (p->next->next->next == 0)
        r += 4;
    /* write through a chained pointer, then read it back */
    p->next->next->value = 40;
    if (c.value == 40)
        r += 8;
    /* walk the list with a loop, summing values (1 + 2 + 40 = 43) */
    {
        int sum = 0;
        struct node *n;
        for (n = p; n != 0; n = n->next)
            sum += n->value;
        if (sum == 43)
            r += 16;
    }

    return (r == 31) ? 42 : r;
}
