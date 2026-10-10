/* Passing a struct to a function by value.  The callee receives an independent
   copy: mutating the parameter must not touch the caller's object, and the
   fields must arrive intact across a few shapes (two ints, one int, three
   fields, and a scalar before and after the struct).  Self-checking, 42.

   Runs on every backend.  On MIPS o32 a struct with 8-byte-aligned members
   (an embedded double or long long) cannot ride the integer word path and is
   rejected at compile time; the word-sized integer members here are fine. */

struct pair {
    int a;
    int b;
};

struct trip {
    int a;
    int b;
    int c;
};

static int
sum_pair(struct pair p)
{
    p.a += 100;             /* mutate the copy; the caller must not see it */
    return p.a + p.b - 100;
}

static int
one_field(struct pair p)
{
    return p.a;
}

static int
sum_trip(struct trip t)
{
    return t.a + t.b + t.c;
}

static int
mix(int lead, struct pair p, int tail)
{
    return lead + p.a + p.b + tail;
}

int
main(void)
{
    struct pair p = { 10, 20 };
    if (sum_pair(p) != 30)
        return 1;
    if (p.a != 10)              /* value semantics: caller unchanged */
        return 2;
    if (one_field(p) != 10)
        return 3;

    struct trip t = { 3, 4, 5 };
    if (sum_trip(t) != 12)
        return 4;

    if (mix(1, p, 2) != 33)     /* 1 + 10 + 20 + 2 */
        return 5;

    return 42;
}
