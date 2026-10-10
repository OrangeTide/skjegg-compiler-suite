/* A struct or union with an anonymous (unnamed) struct/union member keeps a
   field with no name in its field list.  Resolving a named sibling member then
   scans past that unnamed field; an unguarded name compare dereferenced the
   null name and crashed.  A named sibling before or after an anonymous member,
   through a value or a pointer, and in a constant address fold, must all
   resolve cleanly.  (Accessing the anonymous member's own inner fields is a
   separate, unimplemented feature and is not exercised here.)  Returns 42. */

struct outer {
    struct { int ia; int ib; };     /* anonymous member, scanned but not named */
    int tag;
    union { int u0; int u1; };       /* a second anonymous member */
    int mark;
};

/* a constant address fold past the anonymous members (the offsetof idiom) */
static int tag_off = (int)(long)&((struct outer *)0)->tag;
static int mark_off = (int)(long)&((struct outer *)0)->mark;

static int
read_tag(struct outer *p)
{
    return p->tag + p->mark;        /* pointer member access */
}

int
main(void)
{
    struct outer o;
    o.tag = 30;
    o.mark = 12;
    if (o.tag + o.mark != 42)       /* value member access */
        return 1;
    if (read_tag(&o) != 42)
        return 2;
    if (tag_off != 8)               /* two ints of the anon struct precede tag */
        return 3;
    if (mark_off != 16)             /* tag (4) + anon union (4) after tag */
        return 4;
    return 42;
}
