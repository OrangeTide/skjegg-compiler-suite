/* libexc_guest32.c : prove libexc built as LP64 host code (EXC_GUEST32)
 * reads and writes the 32-bit guest object model correctly.
 *
 * This is the load-bearing bet of the guest-32 runtime path (the arm64
 * JIT's Excelsior tier): the runtime C is compiled for the 64-bit host,
 * but every shared struct is held at guest width (4-byte words and
 * pointers) and a stored pointer is recovered by zero-extension because the
 * guest arena lives below 4GB. Here the arena is a MAP_32BIT bump region so
 * every allocation is a real low address, exactly as the JIT's low mapping
 * will be. The test drives the value helpers, spawn/image seeding, dispatch
 * lookup, list and record equality, and the freeze/thaw walker, then checks
 * the results the guest would see.
 *
 * Host-only: no cross toolchain, no qemu. Built and run by
 * `make test-libexc-guest32`.
 */

#include "libexc.h"
#include "utf8.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>

/* --- the low-memory bump arena the runtime allocates from --- */

static char *arena_base;
static size_t arena_off;
static size_t arena_cap;

void *
__moo_arena_alloc(int size)
{
    void *p;

    size = (size + 7) & ~7;
    if (arena_off + (size_t)size > arena_cap)
        return 0;
    p = arena_base + arena_off;
    arena_off += (size_t)size;
    return p;
}

/* --- the host binding surface libexc calls into --- */

static char emit_buf[4096];
static int emit_len;

void
__exh_emit(long chan, const char *buf, long len)
{
    (void)chan;
    if (emit_len + len < (long)sizeof(emit_buf)) {
        memcpy(emit_buf + emit_len, buf, (size_t)len);
        emit_len += (int)len;
    }
}

void
__exh_fault(const struct exc_trapdesc *why, long a, long b)
{
    (void)why; (void)a; (void)b;
    /* a real binding exits 70; the test never trips a fault */
    fprintf(stderr, "unexpected fault\n");
    exit(1);
}

/* a tiny in-memory property store for the freeze/thaw round trip */
struct prop { char key[64]; char val[256]; int len; };
static struct prop props[32];
static int nprops;

long
__exh_prop_put(long obj, const char *key, const char *val)
{
    (void)obj;
    for (int i = 0; i < nprops; i++)
        if (strcmp(props[i].key, key) == 0) {
            strncpy(props[i].val, val, sizeof props[i].val - 1);
            props[i].len = (int)strlen(props[i].val);
            return 0;
        }
    if (nprops < (int)(sizeof props / sizeof props[0])) {
        strncpy(props[nprops].key, key, sizeof props[nprops].key - 1);
        strncpy(props[nprops].val, val, sizeof props[nprops].val - 1);
        props[nprops].len = (int)strlen(props[nprops].val);
        nprops++;
    }
    return 0;
}

long
__exh_prop_get(long obj, const char *key, char *buf, long bufsz)
{
    (void)obj;
    for (int i = 0; i < nprops; i++)
        if (strcmp(props[i].key, key) == 0) {
            long n = props[i].len < bufsz ? props[i].len : bufsz;
            memcpy(buf, props[i].val, (size_t)n);
            return n;
        }
    return -1;
}

long __exh_post(long t, const char *b, long n) { (void)t; (void)b; (void)n; return 0; }
void __exh_yield(void) {}
void __exh_sleep(long ms) { (void)ms; }

/* the selector table exc_sel_by_name reads; the test never resolves a name,
 * so an empty table suffices (it just has to link) */
word __exc_selnames[1] = { 0 };

/* value helpers the compiler calls by emission, so they carry no header
 * prototype; declare the ones this test drives */
struct exc_str *__exc_str_concat(struct exc_str *a, struct exc_str *b);
int __exc_str_eq(struct exc_str *a, struct exc_str *b);
int __exc_str_len(struct exc_str *s);
long __exc_str_find(struct exc_str *hay, struct exc_str *needle);
struct exc_list;
long __exc_list_contains(struct exc_list *l, word v);
word *__exc_rec_new(long nwords);
long __exc_rec_eq(word *a, word *b, long n, long strmask);

static int failures;

static void
check(const char *what, int ok)
{
    if (!ok) {
        printf("FAIL  %s\n", what);
        failures++;
    } else {
        printf("ok    %s\n", what);
    }
}

/* a guest string built in the low arena, laid out at guest width */
static struct exc_str *
gstr(const char *s)
{
    int n = (int)strlen(s);
    struct exc_str *r = __moo_arena_alloc((int)sizeof *r);
    char *buf = __moo_arena_alloc(n + 1);
    memcpy(buf, s, (size_t)n + 1);
    r->len = n;
    EXC_SETDATA(r, buf);
    return r;
}

int
main(void)
{
    /* map the arena low so every guest pointer fits 32 bits */
    arena_cap = 1u << 20;
    arena_base = mmap(0, arena_cap, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (arena_base == MAP_FAILED) {
        perror("mmap MAP_32BIT");
        return 1;
    }
    check("arena is low (< 4GB)", (uintptr_t)arena_base < 0x100000000ull);

    /* the guest layout the compiler emits: 4-byte words and pointers */
    check("sizeof(exc_str) == 8", sizeof(struct exc_str) == 8);
    check("sizeof(word) == 4", sizeof(word) == 4);
    check("sizeof(exc_gptr) == 4", sizeof(exc_gptr) == 4);

    /* strings: build, concat, equality, code-point length */
    {
        struct exc_str *a = gstr("caf\xc3\xa9");   /* "café", 5 bytes, 4 cps */
        struct exc_str *b = gstr("!");
        struct exc_str *ab = __exc_str_concat(a, b);
        struct exc_str *ref = gstr("caf\xc3\xa9!");

        check("str_len counts code points", __exc_str_len(a) == 4);
        check("str_concat length", ab->len == 6);
        check("str_concat content", __exc_str_eq(ab, ref));
        check("str_eq distinguishes", !__exc_str_eq(a, b));
        check("str_find substring", __exc_str_find(ref, b) == 6);
    }

    /* spawn seeds an object's field segment from the class image */
    {
        word *image = __moo_arena_alloc(2 * (int)sizeof(word));
        struct class_desc *cls = __moo_arena_alloc((int)sizeof *cls);
        struct exc_obj *o;

        image[0] = 42;
        image[1] = 99;
        cls->parent = EXC_TOG((void *)0);
        cls->nverbs = 0;
        cls->nwords = 2;
        cls->image = EXC_TOG(image);

        o = __exc_spawn(cls);
        check("spawn seeds field 0 from image", o->fields[0] == 42);
        check("spawn seeds field 1 from image", o->fields[1] == 99);
        check("spawn wrote a low cls handle",
              EXC_CLS(o) == cls && (uint32_t)o->cls == (uintptr_t)cls);

        /* dispatch lookup on an unknown selector walks the chain and misses,
         * exercising EXC_CLS / EXC_PARENT / verbs[] without a code call */
        check("send to unknown selector is a no-op", __exc_send(o, 7, 0, 0) == 0);
        check("send to nil receiver is a no-op", __exc_send(0, 0, 0, 0) == 0);
    }

    /* lists: a guest list is { count, elem[] } at guest width */
    {
        struct { word count; word elem[3]; } *l =
            __moo_arena_alloc((int)sizeof *l);
        l->count = 3;
        l->elem[0] = 10; l->elem[1] = 20; l->elem[2] = 30;
        check("list_contains hit", __exc_list_contains((void *)l, 20) == 1);
        check("list_contains miss", __exc_list_contains((void *)l, 25) == 0);
    }

    /* record value equality over the flattened words, with a str field */
    {
        word *r1 = __exc_rec_new(2);
        word *r2 = __exc_rec_new(2);
        r1[0] = 5; r1[1] = (word)(uintptr_t)gstr("hi");
        r2[0] = 5; r2[1] = (word)(uintptr_t)gstr("hi");   /* distinct copy */
        check("rec_eq compares str field by content",
              __exc_rec_eq(r1, r2, 2, 0x2) == 1);
        r2[0] = 6;
        check("rec_eq sees a differing word", __exc_rec_eq(r1, r2, 2, 0x2) == 0);
    }

    /* freeze/thaw: serialize a field segment through properties and back */
    {
        /* a class with a word field and a str field, its field table after
         * the (zero) verb pairs: nfields, then {name, woffset, kind}* */
        word *nm_hp = __moo_arena_alloc((int)sizeof(struct exc_str));
        (void)nm_hp;
        struct exc_str *fn_hp = gstr("hp");
        struct exc_str *fn_tag = gstr("tag");
        /* class_desc with a 7-word verbs[] tail: nfields=2, two field rows */
        struct class_desc *cls =
            __moo_arena_alloc((int)sizeof(struct class_desc) + 7 * (int)sizeof(word));
        struct exc_obj *o;

        cls->parent = EXC_TOG((void *)0);
        cls->nverbs = 0;
        cls->nwords = 2;
        cls->image = EXC_TOG((void *)0);
        cls->verbs[0] = 2;                                   /* nfields */
        cls->verbs[1] = (word)(uintptr_t)fn_hp;              /* &name */
        cls->verbs[2] = 0;                                   /* woffset */
        cls->verbs[3] = EXC_FK_WORD;                         /* kind */
        cls->verbs[4] = (word)(uintptr_t)fn_tag;
        cls->verbs[5] = 1;
        cls->verbs[6] = EXC_FK_STR;

        o = __exc_spawn(cls);
        o->fields[0] = 77;
        o->fields[1] = (word)(uintptr_t)gstr("boss");

        exc_freeze(o, 1);

        /* a fresh object of the same class, thawed from the saved properties */
        struct exc_obj *o2 = __exc_spawn(cls);
        exc_thaw(o2, 1);
        check("thaw restores the word field", o2->fields[1 - 1] == 77);
        check("thaw restores the str field",
              __exc_str_eq(EXC_G(o2->fields[1], struct exc_str *), gstr("boss")));
    }

    printf("\n%s\n", failures ? "FAILED" : "all guest-32 checks passed");
    return failures ? 1 : 0;
}
