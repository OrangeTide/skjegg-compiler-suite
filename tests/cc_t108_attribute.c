/* GNU __attribute__((...)) is parsed and ignored by skj-cc: it carries only
   optimization and layout hints, none of which skj-cc honours, so dropping the
   whole __attribute__(...) run lets it compile sources (such as the freestanding
   Kobold runtime) that annotate declarations. This exercises the attribute in
   several positions: after a function declarator, on a global, after a struct
   definition, on a local, and a run of two. It stays a normal program otherwise
   and returns 42. */

int early_exit(int code) __attribute__((noreturn));

int __attribute__((unused)) g = 40;

struct pt { int x; int y; } __attribute__((packed));

static int __attribute__((always_inline)) __attribute__((unused))
add2(int a, int b)
{
    return a + b;
}

int
main(void)
{
    struct pt p __attribute__((aligned(8))) = { g, 2 };
    return add2(p.x, p.y);
}
