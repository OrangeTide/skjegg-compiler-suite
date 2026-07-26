/* exc_host.c : minimal in-process test host for the Excelsior ABI.
 *
 * Implements the send primitive, spawn with per-instance field segments, and
 * the bootstrap entry from host-abi.md, enough to run self, inherited,
 * overridden, and true cross-object sends (to a spawned actor of another
 * class) under qemu, without the scheduler or paging. One address space.
 *
 * The compiler emits, per class, a descriptor:
 *     struct class_desc { parent; nverbs; nwords; image;
 *                         (selector, code) * nverbs; }
 * where nwords sizes the object's field segment and image seeds it.
 * and, for the entry class, __exc_entry_class (a pointer to its descriptor)
 * and __exc_entry_selector (the entry verb's selector). __exc_send resolves
 * a selector against the receiver's descriptor chain and calls the verb.
 *
 * Cross-compiled with the m68k toolchain (freestanding, no libc), like
 * pascal_rt.c, and linked with start.S which calls this main().
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "utf8.h"        /* vendored decoder for code-point string ops (R7) */

typedef long word;

/* Fault reporting (excelsior/runtime-errors.md). The compiler emits one
 * { kind, line, &file, &name } descriptor per trap site and calls
 * __exc_trap(desc, a, b) with up to two kind-specific runtime words.
 * Kinds are kept in sync with excelsior/lower.c. */
#define EXC_TRAP_NO_BRANCH   1
#define EXC_TRAP_INDEX_RANGE 2
#define EXC_TRAP_UNCONSUMED  3
#define EXC_TRAP_DIV_ZERO    4
#define EXC_TRAP_OVERFLOW    5

struct exc_trapdesc {
    int kind;
    int line;
    const char *file;           /* NUL-terminated, or 0 (host-detected) */
    const char *name;           /* the failing callee etc., or 0 */
};

void __exc_trap(const struct exc_trapdesc *why, int a, int b);
extern void *__moo_arena_alloc(int size);

/* decimal is base-10 fixed-point: value x 10^-4 in an int32 (numbers.md) */
#define DEC_SCALE 10000

struct class_desc {
    struct class_desc *parent;
    long nverbs;
    long nwords;                /* field-segment size, in words */
    const word *image;          /* initial field values, or 0 for all-zero */
    long verbs[];               /* nverbs pairs: [selector, code] */
};

/* An object is its header followed by its own field segment, so `self.field`
 * is one load of the handle plus a constant offset, and two instances of a
 * class never share storage. The segment is laid out parent fields first, so
 * an inherited field keeps its offset in a subclass. */
struct exc_obj {
    struct class_desc *cls;
    word fields[];
};

#define EXC_OBJ_HDR ((long)sizeof(struct exc_obj))

/* The running actor handle. Compiled code reads it for `self`; the host
 * sets it before entering a verb (here, once, for the bootstrap actor). */
struct exc_obj *__exc_self;

/* Emitted by the compiler for the entry class (the one with `main`). */
extern struct class_desc *__exc_entry_class;
extern long __exc_entry_selector;

/* Call a verb with argc words from argv. Excelsior's calling convention
 * (args pushed right to left, caller pops, result in d0) matches the m68k
 * C convention, so a plain indirect call marshals correctly. */
static word
call_verb(void *code, long argc, word *argv)
{
    switch (argc) {
    case 0: return ((word (*)(void))code)();
    case 1: return ((word (*)(word))code)(argv[0]);
    case 2: return ((word (*)(word, word))code)(argv[0], argv[1]);
    case 3: return ((word (*)(word, word, word))code)(
                       argv[0], argv[1], argv[2]);
    case 4: return ((word (*)(word, word, word, word))code)(
                       argv[0], argv[1], argv[2], argv[3]);
    default: return 0;          /* the test host caps arity at 4 */
    }
}

/* The one dispatch primitive. Walk the receiver's class descriptor and its
 * parent chain; a subclass entry for a selector is found before the
 * parent's, giving override and inheritance. */
/* __exc_spawn(class_desc) -> obj : create an actor of the given class, with
 * its own field segment allocated behind its header and seeded from the
 * class's initial image (the compiler's `Class__image`, which carries each
 * field's default, inherited defaults included). A class with no fields has
 * nwords 0 and no image. */
struct exc_obj *
__exc_spawn(struct class_desc *cls)
{
    struct exc_obj *o;
    long i;

    o = __moo_arena_alloc((int)(EXC_OBJ_HDR + cls->nwords * (long)sizeof(word)));
    if (!o)
        return 0;               /* out of arena: nil */
    o->cls = cls;
    for (i = 0; i < cls->nwords; i++)
        o->fields[i] = cls->image ? cls->image[i] : 0;
    return o;
}

word
__exc_send(struct exc_obj *recv, long selector, long argc, word *argv)
{
    struct class_desc *c;
    long i;

    if (!recv || !recv->cls)
        return 0;               /* nil receiver: no-op (host policy later) */
    for (c = recv->cls; c; c = c->parent)
        for (i = 0; i < c->nverbs; i++)
            if (c->verbs[2 * i] == selector) {
                /* the receiver is the running actor for the duration of the
                 * verb: fields are per-instance, so `self.field` reads this
                 * object's segment. Saved and restored, since the sender
                 * resumes when the verb returns. */
                struct exc_obj *caller = __exc_self;
                word r;

                __exc_self = recv;
                r = call_verb((void *)c->verbs[2 * i + 1], argc, argv);
                __exc_self = caller;
                return r;
            }
    return 0;                   /* selector not understood */
}


/* Box a word-sized maybe payload (fallible.md): a maybe int/bool/fixed
 * at rest is a pointer to its boxed word, or 0 for nothing. Boxing
 * happens only on capture; expression flow never allocates. */
word *
__exc_box(word v)
{
    word *p = __moo_arena_alloc(4);
    *p = v;
    return p;
}

/* Decimal (base-10 fixed-point, numbers.md) multiply and divide. A
 * decimal value is a 32-bit integer holding real x 10^4; the product or
 * quotient needs a 64-bit intermediate and a rescale, rounded half away
 * from zero, with a result outside int32 an OVERFLOW fault. The compiler
 * emits add, subtract, compare, negate, and modulo inline (they are the
 * plain int ops on the scaled value) and calls these two for the
 * operations that move the scale. The 64-bit divides resolve to the
 * libgcc-style helpers in start.S. */
word
__exc_decmul(word a, word b)
{
    long long r = (long long)a * (long long)b;

    r = (r >= 0) ? (r + DEC_SCALE / 2) / DEC_SCALE      /* half away */
                 : (r - DEC_SCALE / 2) / DEC_SCALE;
    if (r > 0x7fffffffLL || r < -0x80000000LL) {
        static const struct exc_trapdesc why =
            { EXC_TRAP_OVERFLOW, 0, 0, "decimal multiply" };
        __exc_trap(&why, 0, 0);
    }
    return (word)r;
}

word
__exc_decdiv(word a, word b)
{
    long long n, q, rem, ab;

    if (b == 0) {                   /* defensive: compiled code checks */
        static const struct exc_trapdesc why =
            { EXC_TRAP_DIV_ZERO, 0, 0, "decimal divide" };
        __exc_trap(&why, 0, 0);
    }
    n = (long long)a * DEC_SCALE;
    q = n / b;
    rem = n % b;
    if (rem < 0)
        rem = -rem;
    ab = (b < 0) ? -(long long)b : (long long)b;
    if (2 * rem >= ab)              /* round half away from zero */
        q += ((a < 0) != (b < 0)) ? -1 : 1;
    if (q > 0x7fffffffLL || q < -0x80000000LL) {
        static const struct exc_trapdesc why =
            { EXC_TRAP_OVERFLOW, 0, 0, "decimal divide" };
        __exc_trap(&why, 0, 0);
    }
    return (word)q;
}

/* Strings. A string value is a pointer to this descriptor; the compiler
 * emits literals as { len, &bytes } and routes concat/equality here. New
 * strings are bump-allocated from the arena in start.S (immutable, no free).
 * Ported from runtime/str.c (the MooScript string runtime). */
struct exc_str {
    int len;
    const char *data;
};

struct exc_str *
__exc_str_concat(struct exc_str *a, struct exc_str *b)
{
    struct exc_str *r;
    char *buf;
    int len, i;

    if (!a)
        return b;
    if (!b)
        return a;
    len = a->len + b->len;
    /* An owned buffer carries a trailing NUL so it demotes to a C char*
     * for free (string-repr.md, the owned invariant): alloc len + 1 and
     * terminate. Only views (slices) stay length-only. */
    buf = __moo_arena_alloc(len + 1);
    for (i = 0; i < a->len; i++)
        buf[i] = a->data[i];
    for (i = 0; i < b->len; i++)
        buf[a->len + i] = b->data[i];
    buf[len] = '\0';
    r = __moo_arena_alloc(sizeof *r);
    r->len = len;
    r->data = buf;
    return r;
}

int
__exc_str_eq(struct exc_str *a, struct exc_str *b)
{
    int i;

    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    if (a->len != b->len)
        return 0;
    for (i = 0; i < a->len; i++)
        if (a->data[i] != b->data[i])
            return 0;
    return 1;
}

/* lexicographic byte order: <0, 0, >0. Bytes compare unsigned; a proper
 * prefix sorts before the longer string. */
int
__exc_str_cmp(struct exc_str *a, struct exc_str *b)
{
    int i, n;

    if (a == b)
        return 0;
    if (!a)
        return (b && b->len) ? -1 : 0;
    if (!b)
        return a->len ? 1 : 0;
    n = a->len < b->len ? a->len : b->len;
    for (i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a->data[i];
        unsigned char cb = (unsigned char)b->data[i];
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (a->len == b->len)
        return 0;
    return a->len < b->len ? -1 : 1;
}

/* text-encoding.md (R7): the four character operations count code points, not
 * bytes. Storage stays UTF-8 { byte-len, data }; concat/equality/ordering stay
 * byte-level (already correct); only length, index, slice, and `for c in s`
 * decode. `length(s)` is `__exc_str_len` (code-point count); `bytes(s)` reads
 * word 0 (the byte length) inline in the compiler. utf8_decode is lenient: a
 * bad or truncated byte counts as one unit and never faults (D4). */

/* length(s): the number of code points. */
int
__exc_str_len(struct exc_str *s)
{
    int i = 0, count = 0;
    uint32_t rune;

    if (!s)
        return 0;
    while (i < s->len) {
        int n = utf8_decode(&rune, (const unsigned char *)s->data + i,
                            (size_t)(s->len - i));
        if (n <= 0)
            n = 1;              /* defensive: never stall */
        i += n;
        count++;
    }
    return count;
}

/* s[lo..hi], 1-based inclusive over CODE POINTS, clamped to the string. An
 * empty or reversed range (or a start past the end) yields the empty string.
 * The result shares s's buffer (no copy): a code-point slice is a byte range,
 * and strings are immutable, so aliasing the parent is safe. */
struct exc_str *
__exc_str_slice(struct exc_str *s, int lo, int hi)
{
    struct exc_str *r = __moo_arena_alloc(sizeof *r);
    int i = 0, k = 0;           /* byte offset, code points seen */
    int startb = 0, endb = 0;   /* byte span of the [lo,hi] code points */
    uint32_t rune;

    if (!s) {
        r->len = 0;
        r->data = (const char *)0;
        return r;
    }
    if (lo < 1)
        lo = 1;
    while (i < s->len) {
        int n = utf8_decode(&rune, (const unsigned char *)s->data + i,
                            (size_t)(s->len - i));
        if (n <= 0)
            n = 1;
        k++;                    /* this is code point k (1-based), at byte i */
        if (k == lo)
            startb = i;
        if (k <= hi)
            endb = i + n;       /* extend the end while within hi */
        i += n;
    }
    if (lo > k || lo > hi) {    /* start past the last code point, or reversed */
        r->len = 0;
        r->data = s->data;
        return r;
    }
    r->len = endb - startb;     /* hi past the end clamps: endb is the last byte */
    r->data = s->data + startb;
    return r;
}

/* s[i], 1-based: the one-code-point string at code-point position i (empty if
 * out of range; the fallible `s[i]` bounds-checks against the code-point count
 * before calling this). */
struct exc_str *
__exc_str_at(struct exc_str *s, int i)
{
    return __exc_str_slice(s, i, i);
}

/* The author channel (output.md). The compiler routes `trace x` (compiled
 * only under -t) to one of these by the static type of x; each writes one
 * value plus a newline to stderr. Player-facing text goes through `tell`
 * (the console object below); the old `log()` builtin retired into these
 * two. */

extern int write(int fd, const char *buf, int n);
extern void exit(int code);

static int
fmt_int(long v, char *buf)
{
    char tmp[16];
    unsigned long u;
    int t = 0, n = 0, neg = 0;

    if (v < 0) {
        neg = 1;
        u = (unsigned long)(-(v + 1)) + 1UL;    /* negate without overflow */
    } else {
        u = (unsigned long)v;
    }
    if (u == 0)
        tmp[t++] = '0';
    while (u) {
        tmp[t++] = (char)('0' + (int)(u % 10));
        u /= 10;
    }
    if (neg)
        buf[n++] = '-';
    while (t)
        buf[n++] = tmp[--t];
    return n;
}

/* A decimal (scaled x 10^4) prints exactly: whole part, then up to four
 * fraction digits with trailing zeros stripped; a whole value prints
 * bare (numbers.md). */
static int
fmt_dec(long v, char *buf)
{
    unsigned long u, frac;
    int n = 0, len;
    char f[4];

    if (v < 0) {
        buf[n++] = '-';
        u = (unsigned long)(-(v + 1)) + 1UL;    /* negate without overflow */
    } else {
        u = (unsigned long)v;
    }
    n += fmt_int((long)(u / DEC_SCALE), buf + n);
    frac = u % DEC_SCALE;
    if (frac == 0)
        return n;
    f[0] = (char)('0' + frac / 1000 % 10);
    f[1] = (char)('0' + frac / 100 % 10);
    f[2] = (char)('0' + frac / 10 % 10);
    f[3] = (char)('0' + frac % 10);
    len = 4;
    while (len > 1 && f[len - 1] == '0')
        len--;
    buf[n++] = '.';
    for (int i = 0; i < len; i++)
        buf[n++] = f[i];
    return n;
}

/****************************************************************
 * Fault reporting (runtime-errors.md). A fault report teaches like
 * a compile error: kind, location, the values involved, and a hint.
 * Written to stderr; the exit code stays 70.
 ****************************************************************/

static void
eputs(const char *s)
{
    int n = 0;
    while (s[n])
        n++;
    write(2, s, n);
}

static void
eputi(long v)
{
    char buf[16];
    write(2, buf, fmt_int(v, buf));
}

static void
eploc(const struct exc_trapdesc *why)
{
    if (why && why->file) {
        eputs(why->file);
        eputs(" line ");
        eputi(why->line);
        eputs(": ");
    }
}

void
__exc_trap(const struct exc_trapdesc *why, int a, int b)
{
    eputs("fault: ");
    eploc(why);
    switch (why ? why->kind : 0) {
    case EXC_TRAP_NO_BRANCH:
        eputs("no branch chosen (the subject was ");
        eputi(a);
        eputs(") and there is no `else`\n"
              "hint: add an `else`, or cover the value with a label\n");
        break;
    case EXC_TRAP_INDEX_RANGE:
        eputs("index ");
        eputi(a);
        eputs(" is out of range (1 to ");
        eputi(b);
        eputs(")\n"
              "hint: give it a fallback with `else`, or branch with"
              " `if var`\n");
        break;
    case EXC_TRAP_UNCONSUMED:
        if (why->name) {
            eputs("`");
            eputs(why->name);
            eputs("` produced no value");
        } else {
            eputs("a fallible expression produced no value");
        }
        eputs(" and nothing consumed the failure\n"
              "hint: catch it with `else` or `if var`, or store it in"
              " a `maybe`\n");
        break;
    case EXC_TRAP_DIV_ZERO:
        if (why->name) {
            eputs("`");
            eputs(why->name);
            eputs("`: ");
        }
        eputs("division by zero\n"
              "hint: guard the divisor, or give the division a fallback"
              " with `else`\n");
        break;
    case EXC_TRAP_OVERFLOW:
        if (why->name) {
            eputs("`");
            eputs(why->name);
            eputs("`: ");
        }
        eputs("arithmetic overflow: the result does not fit\n"
              "hint: ints hold about +/- 2.1 billion; decimal holds"
              " about +/- 214748.3647\n");
        break;
    default:
        eputs("no branch chosen and no else\n");
        break;
    }
    exit(70);
}

/* The R2 trace event: a `match` statement whose subject matched no arm
 * (a designed no-op) reports itself when the build traces (-t). */
void
__exc_trace_nomatch(const struct exc_trapdesc *why, int subj)
{
    eputs("trace: ");
    eploc(why);
    eputs("match chose no arm (the subject was ");
    eputi(subj);
    eputs(")\n");
}

/* fixed 6 fractional digits: deterministic and adequate for test output */
static int
fmt_double(double v, char *buf)
{
    long ip;
    double frac;
    int n = 0, i;

    if (v != v) {                       /* NaN */
        buf[0] = 'n'; buf[1] = 'a'; buf[2] = 'n';
        return 3;
    }
    if (v < 0.0) {
        buf[n++] = '-';
        v = -v;
    }
    v += 0.0000005;                     /* round to 6 places */
    ip = (long)v;
    frac = v - (double)ip;
    n += fmt_int(ip, buf + n);
    buf[n++] = '.';
    for (i = 0; i < 6; i++) {
        int d;
        frac *= 10.0;
        d = (int)frac;
        if (d < 0) d = 0;
        if (d > 9) d = 9;
        buf[n++] = (char)('0' + d);
        frac -= d;
    }
    return n;
}

void
__exc_trace_str(struct exc_str *s)
{
    if (s && s->len)
        write(2, s->data, s->len);
    write(2, "\n", 1);
}

void
__exc_trace_int(long v)
{
    char buf[24];
    write(2, buf, fmt_int(v, buf));
    write(2, "\n", 1);
}

void
__exc_trace_float(double v)
{
    char buf[48];
    write(2, buf, fmt_double(v, buf));
    write(2, "\n", 1);
}

void
__exc_trace_dec(long v)
{
    char buf[24];
    write(2, buf, fmt_dec(v, buf));
    write(2, "\n", 1);
}

void
__exc_trace_bool(long v)
{
    write(2, v ? "true\n" : "false\n", v ? 5 : 6);
}

/* String conversions for interpolation holes: the compiler routes each
 * ${expr} through one of these by the static type of expr, and concatenates
 * the results with the literal pieces. Each returns a fresh arena string. */
static struct exc_str *
make_str(const char *s, int n)
{
    struct exc_str *r = __moo_arena_alloc(sizeof *r);
    char *buf = __moo_arena_alloc(n + 1);       /* owned: room for a NUL */
    int i;

    for (i = 0; i < n; i++)
        buf[i] = s[i];
    buf[n] = '\0';                              /* owned buffers terminate */
    r->len = n;
    r->data = buf;
    return r;
}

struct exc_str *
__exc_str_from_int(long v)
{
    char buf[24];
    return make_str(buf, fmt_int(v, buf));
}

struct exc_str *
__exc_str_from_float(double v)
{
    char buf[48];
    return make_str(buf, fmt_double(v, buf));
}

struct exc_str *
__exc_str_from_dec(long v)
{
    char buf[24];
    return make_str(buf, fmt_dec(v, buf));
}

struct exc_str *
__exc_str_from_bool(long v)
{
    return v ? make_str("true", 4) : make_str("false", 5);
}

/* an enum prints its member name (enums.md D6): the ordinal indexes the
 * compiler-emitted per-enum name table of str descriptors */
struct exc_str *
__exc_str_from_enum(long ord, word *names)
{
    return (struct exc_str *)names[ord];
}

/* Lists. A list value is a pointer to { count, elem[count] }, count in word
 * 0 (the same slot a str descriptor uses for its length, so `len` reads word
 * 0 of either). Elements are word-sized: an int/bool, or a pointer for a str
 * element. Lists are immutable: append and set return a fresh list from the
 * arena. Ported from runtime/list.c (the MooScript list runtime). */
struct exc_list {
    long count;
    word elem[];
};

/* The list mutators below are copy-on-write: each allocates a fresh list and
 * copies, so building a list by repeated prepend/insert/append is O(N^2).
 * TODO(someday): when the memory.md refcounting lands, mutate in place if the
 * input list is uniquely owned (refcount 1) rather than copying, the
 * isKnownUniquelyReferenced / transient trick. It is safe only under
 * refcounting; the arena today has none, so every mutator copies. Loop-building
 * should use `buffer of T` (buffer.md) meanwhile. Applies to all of these;
 * prepend and insert benefit most (they shift as well as copy). */
struct exc_list *
__exc_list_append(struct exc_list *l, word v)
{
    long i, oc = l ? l->count : 0;
    struct exc_list *r = __moo_arena_alloc((int)(sizeof(long) + (oc + 1) *
                                                 sizeof(word)));
    r->count = oc + 1;
    for (i = 0; i < oc; i++)
        r->elem[i] = l->elem[i];
    r->elem[oc] = v;
    return r;
}

/* set(l, idx, v): a copy with the 1-based element idx replaced. An out-of-
 * range idx leaves the copy unchanged (the caller can bounds-check first). */
struct exc_list *
__exc_list_set(struct exc_list *l, long idx, word v)
{
    long i, c = l ? l->count : 0;
    struct exc_list *r = __moo_arena_alloc((int)(sizeof(long) +
                                                 c * sizeof(word)));
    r->count = c;
    for (i = 0; i < c; i++)
        r->elem[i] = l->elem[i];
    if (idx >= 1 && idx <= c)
        r->elem[idx - 1] = v;
    return r;
}

/* xs[lo..hi]: a fresh list of the 1-based inclusive range, clamped to the
 * list; an empty or inverted range yields the empty list. */
struct exc_list *
__exc_list_slice(struct exc_list *l, long lo, long hi)
{
    struct exc_list *r;
    long i, nc, c = l ? l->count : 0;

    if (lo < 1)
        lo = 1;
    if (hi > c)
        hi = c;
    if (lo > hi) {
        r = __moo_arena_alloc((int)sizeof(long));
        r->count = 0;
        return r;
    }
    nc = hi - lo + 1;
    r = __moo_arena_alloc((int)(sizeof(long) + nc * sizeof(word)));
    r->count = nc;
    for (i = 0; i < nc; i++)
        r->elem[i] = l->elem[lo - 1 + i];
    return r;
}

/* delete(l, idx): a fresh copy without the 1-based element idx. An out-of-
 * range idx yields an unchanged copy. */
struct exc_list *
__exc_list_delete(struct exc_list *l, long idx)
{
    struct exc_list *r;
    long i, j = 0, c = l ? l->count : 0;
    long nc = (idx >= 1 && idx <= c) ? c - 1 : c;

    r = __moo_arena_alloc((int)(sizeof(long) + nc * sizeof(word)));
    r->count = nc;
    for (i = 0; i < c; i++)
        if (i != idx - 1)
            r->elem[j++] = l->elem[i];
    return r;
}

/* prepend(l, v): a fresh copy with v at the front (list-ops.md). */
struct exc_list *
__exc_list_prepend(struct exc_list *l, word v)
{
    long i, oc = l ? l->count : 0;
    struct exc_list *r = __moo_arena_alloc((int)(sizeof(long) + (oc + 1) *
                                                 sizeof(word)));
    r->count = oc + 1;
    r->elem[0] = v;
    for (i = 0; i < oc; i++)
        r->elem[i + 1] = l->elem[i];
    return r;
}

/* insert(l, idx, v): a fresh copy with v at 1-based position idx, the rest
 * shifted right. idx is clamped to [1, count+1], so idx past the end appends
 * and idx below 1 prepends (list-ops.md). */
struct exc_list *
__exc_list_insert(struct exc_list *l, long idx, word v)
{
    long i, oc = l ? l->count : 0;
    struct exc_list *r = __moo_arena_alloc((int)(sizeof(long) + (oc + 1) *
                                                 sizeof(word)));
    r->count = oc + 1;
    if (idx < 1)
        idx = 1;
    if (idx > oc + 1)
        idx = oc + 1;
    for (i = 0; i < idx - 1; i++)
        r->elem[i] = l->elem[i];
    r->elem[idx - 1] = v;
    for (i = idx - 1; i < oc; i++)
        r->elem[i + 1] = l->elem[i];
    return r;
}

/* reverse(l): a fresh copy in reverse order (list-ops.md). */
struct exc_list *
__exc_list_reverse(struct exc_list *l)
{
    long i, c = l ? l->count : 0;
    struct exc_list *r = __moo_arena_alloc((int)(sizeof(long) +
                                                 c * sizeof(word)));
    r->count = c;
    for (i = 0; i < c; i++)
        r->elem[i] = l->elem[c - 1 - i];
    return r;
}

/* a + b: a fresh list of a's elements followed by b's (list-ops.md, the same
 * `+` that concatenates strings). */
struct exc_list *
__exc_list_concat(struct exc_list *a, struct exc_list *b)
{
    long i, ac = a ? a->count : 0, bc = b ? b->count : 0;
    struct exc_list *r = __moo_arena_alloc((int)(sizeof(long) +
                                                 (ac + bc) * sizeof(word)));
    r->count = ac + bc;
    for (i = 0; i < ac; i++)
        r->elem[i] = a->elem[i];
    for (i = 0; i < bc; i++)
        r->elem[ac + i] = b->elem[i];
    return r;
}

/* Records (records.md): a record value is a pointer to an arena block of its
 * word-sized fields (no header; the compiler knows the field count and each
 * field's offset). Value semantics are preserved by copying the block on
 * assignment or by-value pass; the inline layout of records.md D6 is a deferred
 * optimization, the observable copy-on-assign semantics are identical. */
word *
__exc_rec_new(long nwords)
{
    word *r = __moo_arena_alloc((int)(nwords * (long)sizeof(word)));
    long i;

    for (i = 0; i < nwords; i++)
        r[i] = 0;
    return r;
}

word *
__exc_rec_copy(word *src, long nwords)
{
    word *r = __moo_arena_alloc((int)(nwords * (long)sizeof(word)));
    long i;

    for (i = 0; i < nwords; i++)
        r[i] = src ? src[i] : 0;
    return r;
}

/* copy nwords words from src into an existing location dst (no allocation):
 * a nested record nests flat, so constructing or assigning a record field
 * blits the sub-record's words into the parent's inline slot (records.md) */
void
__exc_rec_blit(word *dst, word *src, long nwords)
{
    long i;

    for (i = 0; i < nwords; i++)
        dst[i] = src ? src[i] : 0;
}

/* record value equality (records.md D5): two records are equal when all their
 * fields are. Records nest flat, so `n` is the flattened word count and the
 * fields (including a nested record's, at their flattened positions) compare
 * word by word: a word field (int/bool/decimal/enum/obj) compares by word,
 * which is value equality for a scalar and correctly identity for an obj
 * handle; a str field (its bit set in strmask) compares by content. A list
 * field is guarded out at compile time for now. */
long
__exc_rec_eq(word *a, word *b, long n, long strmask)
{
    long i;

    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    for (i = 0; i < n; i++) {
        if (strmask & (1L << i)) {
            if (!__exc_str_eq((struct exc_str *)a[i], (struct exc_str *)b[i]))
                return 0;
        } else if (a[i] != b[i])
            return 0;
    }
    return 1;
}

/* `v in list`: membership. Int/bool/fixed elements compare by word; str
 * elements compare by content (two equal strings need not be the same
 * pointer), so the compiler picks the _str variant for a list of str. */
long
__exc_list_contains(struct exc_list *l, word v)
{
    long i;

    if (!l)
        return 0;
    for (i = 0; i < l->count; i++)
        if (l->elem[i] == v)
            return 1;
    return 0;
}

long
__exc_list_contains_str(struct exc_list *l, struct exc_str *v)
{
    long i;

    if (!l)
        return 0;
    for (i = 0; i < l->count; i++)
        if (__exc_str_eq((struct exc_str *)l->elem[i], v))
            return 1;
    return 0;
}

/* `p in list` for a list of records: each element is compared to the needle
 * by value (records.md), reusing the flat field compare with the record's
 * flattened word count and str bitmask. */
long
__exc_list_contains_rec(struct exc_list *l, word *v, long nwords, long strmask)
{
    long i;

    if (!l)
        return 0;
    for (i = 0; i < l->count; i++)
        if (__exc_rec_eq((word *)l->elem[i], v, nwords, strmask))
            return 1;
    return 0;
}

/* `needle in haystack`: 1-based position of the first match, or 0 if absent.
 * The compiler tests the result against 0 for the boolean. Ported from
 * runtime/str.c (__moo_str_index). */
long
__exc_str_find(struct exc_str *hay, struct exc_str *needle)
{
    long i, j;

    if (!hay || !needle)
        return 0;
    if (needle->len == 0)
        return 1;
    if (needle->len > hay->len)
        return 0;
    for (i = 0; i <= hay->len - needle->len; i++) {
        long match = 1;
        for (j = 0; j < needle->len; j++)
            if (hay->data[i + j] != needle->data[j]) {
                match = 0;
                break;
            }
        if (match)
            return i + 1;
    }
    return 0;
}

/* The selector-name table the compiler emits: { count, &name0, ... } in
 * selector-id order (output.md; the seed of host-abi.md's selector intern
 * table). The host resolves its native objects' verb names against it. */
extern long __exc_selnames[];

static int
ci_eq(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z')
            ca += 'a' - 'A';
        if (cb >= 'A' && cb <= 'Z')
            cb += 'a' - 'A';
        if (ca != cb)
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static long
sel_by_name(const char *name)
{
    long n = __exc_selnames[0];

    for (long i = 0; i < n; i++)
        if (ci_eq((const char *)__exc_selnames[1 + i], name))
            return i;
    return -1;
}

/* The console: a host-native player object whose tell(msg) writes the
 * string plus a newline to stdout. The bootstrap hands it to a
 * main(player is obj) entry verb (output.md), so output tests exercise
 * the real send path end to end. */
static word
console_tell(struct exc_str *msg)
{
    if (msg && msg->len)
        write(1, msg->data, msg->len);
    write(1, "\n", 1);
    return 0;
}

/* The console is native, so it has no fields: nwords 0, no image. */
static struct {
    struct class_desc *parent;
    long nverbs;
    long nwords;
    const word *image;
    long verbs[2];
} console_desc = { 0, 0, 0, 0, { 0, 0 } };

static struct exc_obj console_obj = { (struct class_desc *)&console_desc };

extern long __exc_entry_argc;

int
main(void)
{
    word argv[1];
    long argc = 0;
    long tell = sel_by_name("tell");
    struct exc_obj *bootstrap;

    if (tell >= 0) {                /* the module knows `tell` */
        console_desc.nverbs = 1;
        console_desc.verbs[0] = tell;
        console_desc.verbs[1] = (long)console_tell;
    }
    /* the entry actor is spawned like any other, so it gets its own field
     * segment seeded from its class image */
    bootstrap = __exc_spawn(__exc_entry_class);
    __exc_self = bootstrap;
    if (__exc_entry_argc >= 1) {
        argv[0] = (word)&console_obj;
        argc = 1;
    }
    return (int)__exc_send(bootstrap, __exc_entry_selector, argc, argv);
}
