/* exc_walker_test.c : self-checking qemu test for the freeze/thaw
 * walker (host-abi.md D7).
 *
 * Links the compiled fixture class (tests/exs_walker_fixture.exs, so
 * the descriptor's field table is the compiler's real output), libexc,
 * and this driver, which is its own minimal binding with an in-memory
 * property store. Spawns a Chest, mutates fields through the field
 * table, freezes to the store, checks the delta rule (a default-valued
 * field writes no property), thaws a fresh instance, and compares.
 * Exits 0 on success; any failure names itself on stderr and exits 1.
 *
 * Cross-compiled with the m68k toolchain and run under qemu-m68k by
 * `make test-exc-walker`.
 *
 * Made by a machine. PUBLIC DOMAIN (CC0-1.0)
 */

#include "../runtime/libexc.h"

extern struct class_desc Chest__desc;
extern int write(int fd, const char *buf, int n);
extern void exit(int code);

/* ---- the driver's binding: an in-memory property store ---- */

#define NSLOTS 32

static struct {
    char key[64];
    char val[256];
    long len;
    int used;
} store[NSLOTS];

static int
str_same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

void
__exh_emit(long chan, const char *buf, long len)
{
    (void)chan;
    write(2, buf, (int)len);
}

void
__exh_fault(const struct exc_trapdesc *why, long a, long b)
{
    (void)why;
    (void)a;
    (void)b;
    exit(70);
}

long
__exh_prop_put(long obj, const char *key, const char *val)
{
    int i, free_slot = -1;

    (void)obj;
    for (i = 0; i < NSLOTS; i++) {
        if (store[i].used && str_same(store[i].key, key))
            break;
        if (!store[i].used && free_slot < 0)
            free_slot = i;
    }
    if (i == NSLOTS) {
        if (free_slot < 0)
            return -1;
        i = free_slot;
    }
    store[i].used = 1;
    {
        int n = 0;
        while (key[n] && n < 63) {
            store[i].key[n] = key[n];
            n++;
        }
        store[i].key[n] = 0;
        n = 0;
        while (val[n] && n < 255) {
            store[i].val[n] = val[n];
            n++;
        }
        store[i].len = n;
    }
    return 0;
}

long
__exh_prop_get(long obj, const char *key, char *buf, long bufsz)
{
    (void)obj;
    for (int i = 0; i < NSLOTS; i++) {
        if (!store[i].used || !str_same(store[i].key, key))
            continue;
        {
            long n = store[i].len < bufsz ? store[i].len : bufsz;
            for (long j = 0; j < n; j++)
                buf[j] = store[i].val[j];
            return n;
        }
    }
    return -1;
}

long
__exh_post(long target, const char *buf, long len)
{
    (void)target; (void)buf; (void)len;
    return -1;
}

void
__exh_yield(void)
{
}

void
__exh_sleep(long ms)
{
    (void)ms;
}

/* ---- the checks ---- */

static void
fail(const char *msg)
{
    int n = 0;

    while (msg[n])
        n++;
    write(2, msg, n);
    write(2, "\n", 1);
    exit(1);
}

/* find a field's table entry slot in Chest's own table by name */
static const long *
field_entry(const char *name)
{
    const long *ft = Chest__desc.verbs + 2 * Chest__desc.nverbs;
    long nf = ft[0];

    for (long i = 0; i < nf; i++)
        if (str_same((const char *)ft[1 + 3 * i], name))
            return ft + 1 + 3 * i;
    fail("field not in table");
    return 0;
}

static long
field_off(const char *name)
{
    return field_entry(name)[1];
}

static int
in_store(const char *key)
{
    for (int i = 0; i < NSLOTS; i++)
        if (store[i].used && str_same(store[i].key, key))
            return 1;
    return 0;
}

static const char gold[] = "gold";
static struct exc_str gold_str = { 4, gold };

int
main(void)
{
    long off_hp = field_off("hp");
    long off_label = field_off("label");
    long off_open = field_off("open");
    long off_count = field_off("count");
    struct exc_obj *o1, *o2;

    o1 = __exc_spawn(&Chest__desc);
    if (!o1)
        fail("spawn failed");
    if (o1->fields[off_hp] != 10)
        fail("default hp wrong");

    if (field_entry("stash")[2] != EXC_FK_MAYBE)
        fail("maybe field not marked EXC_FK_MAYBE");

    /* an obj field is a handle, not stable across a freeze, so it is
     * marked EXC_FK_OBJ and the walker must never persist it as a word */
    if (field_entry("owner")[2] != EXC_FK_OBJ)
        fail("obj field not marked EXC_FK_OBJ");

    o1->fields[off_hp] = 77;
    o1->fields[off_label] = (word)&gold_str;
    o1->fields[off_open] = 1;
    /* count stays at its default 0: full-write freeze stores it too */

    exc_freeze(o1, 7);

    if (!in_store("x_hp") || !in_store("x_label") || !in_store("x_open"))
        fail("freeze missed a changed field");
    if (!in_store("x_count"))
        fail("freeze skipped a walked field (delta is unsound)");
    if (in_store("x_stash"))
        fail("freeze wrote a maybe field (a raw pointer)");
    if (in_store("x_owner"))
        fail("freeze wrote an obj field (a raw handle)");

    o2 = __exc_spawn(&Chest__desc);
    exc_thaw(o2, 7);

    if (o2->fields[off_hp] != 77)
        fail("thawed hp wrong");
    if (o2->fields[off_open] != 1)
        fail("thawed open wrong");
    if (o2->fields[off_count] != 0)
        fail("thawed count not default");
    {
        struct exc_str *s = (struct exc_str *)o2->fields[off_label];
        if (!s || s->len != 4 || s->data[0] != 'g' || s->data[3] != 'd')
            fail("thawed label wrong");
    }

    /* the revert scenario the delta rule got wrong: back to the default,
     * freeze again, and the earlier x_hp=77 must not resurrect */
    o2->fields[off_hp] = 10;
    exc_freeze(o2, 7);
    {
        struct exc_obj *o3 = __exc_spawn(&Chest__desc);
        exc_thaw(o3, 7);
        if (o3->fields[off_hp] != 10)
            fail("stale property resurrected a reverted field");
    }

    write(1, "walker ok\n", 10);
    return 0;
}
