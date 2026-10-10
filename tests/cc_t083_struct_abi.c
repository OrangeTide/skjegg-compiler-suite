/* cc_t083_struct_abi.c : by-value struct passing and return across the register
   psABI boundary.  Pins the RISC-V ilp32 edges (1-word struct in one register,
   2-word struct in two, a >8-byte struct passed by reference, a struct argument
   after enough scalars to spill, a struct with a float field, and struct
   returns of each size) while staying correct on x86-64 and arm64 too.  Avoids
   bare `long` so sizes do not shift between ILP32 and LP64.  Self-checking:
   returns 42.  psABI struct targets only (excluded on ColdFire and MIPS). */

struct one { int a; };                  /* 4 bytes: one register word */
struct two { int a, b; };               /* 8 bytes: two register words */
struct three { int a, b, c; };          /* 12 bytes: by reference on RV ilp32 */
struct fmix { float f; int n; };        /* a float field rides a gp reg on RV */

static int
sum_one(struct one s)
{
    return s.a;
}

static int
sum_two(struct two s)
{
    return s.a + s.b;
}

static int
sum_three(struct three s)
{
    return s.a + s.b + s.c;
}

/* a struct argument after seven scalar words: on RV a0..a6 are taken, so the
   two-word struct splits (one word in a7, one on the stack) */
static int
tail(int p1, int p2, int p3, int p4, int p5, int p6, int p7, struct two s)
{
    return p1 + p2 + p3 + p4 + p5 + p6 + p7 + s.a + s.b;
}

static int
fmix_sum(struct fmix s)
{
    return (int)s.f + s.n;
}

static struct two
make_two(int a, int b)
{
    struct two r;
    r.a = a;
    r.b = b;
    return r;                           /* <=8-byte return: a0/a1 */
}

static struct three
make_three(int a, int b, int c)
{
    struct three r;
    r.a = a;
    r.b = b;
    r.c = c;
    return r;                           /* >8-byte return: hidden pointer */
}

int
main(void)
{
    int r = 0;

    struct one o = { 7 };
    if (sum_one(o) == 7) r += 1;

    struct two t = { 10, 20 };
    if (sum_two(t) == 30) r += 2;

    struct three th = { 100, 200, 300 };
    if (sum_three(th) == 600) r += 4;

    if (tail(1, 2, 3, 4, 5, 6, 7, t) == 28 + 30) r += 8;

    struct fmix fm = { 2.5f, 40 };
    if (fmix_sum(fm) == 42) r += 16;

    struct two mt = make_two(3, 9);
    if (mt.a == 3 && mt.b == 9) r += 32;

    struct three m3 = make_three(4, 5, 6);
    if (m3.a == 4 && m3.b == 5 && m3.c == 6) r += 64;

    /* a returned struct fed straight into a by-value call */
    if (sum_two(make_two(11, 12)) == 23) r += 128;

    return (r == 255) ? 42 : r;
}
