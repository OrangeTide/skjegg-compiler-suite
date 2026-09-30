/* test_guest_mmap.c : the guest dynamic-mapping primitives in emu/guest.c
 *
 * A host-side test with no CPU core and no toolchain: it drives guest_mem
 * directly. It pins the guard page (a stack overrun must fault), the
 * release path (gm_munmap frees the region and its guard), the aggregate
 * commit budget (gm_pool), and the guest-range copy-out (gm_read_out).
 *
 * The guard page is the one that matters: it is the safety boundary a
 * guarded coroutine stack relies on, and nothing else in the tree overruns
 * a stack to prove it faults.
 */

#include "guest.h"

#include <stdio.h>
#include <string.h>

#define LIMIT   (16u * 1024u * 1024u)   /* per-guest commit cap */

static int failures;

static void
check_int(const char *what, long got, long want)
{
    if (got == want)
        return;
    printf("FAIL %s: got %ld, want %ld\n", what, got, want);
    failures++;
}

static void
check_str(const char *what, const char *got, const char *want)
{
    if (got && want && strcmp(got, want) == 0)
        return;
    printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got ? got : "(null)",
           want);
    failures++;
}

static void
check_true(const char *what, int cond)
{
    if (cond)
        return;
    printf("FAIL %s\n", what);
    failures++;
}

/****************************************************************
 * A guarded mapping is usable, and the page just below it faults as a
 * stack overflow rather than committing or reading zero.
 ****************************************************************/

static void
test_guard(void)
{
    guest_mem m;
    uint32_t base;

    gm_init(&m, LIMIT);
    base = gm_mmap(&m, 4096, GM_MAP_GUARD_LO);
    check_true("guard: mmap succeeded", base != 0);

    /* The mapping itself is readable and writable. */
    gm_write8(&m, base, 0xAB);
    check_int("guard: mapping holds the write", gm_read8(&m, base), 0xAB);
    check_int("guard: no fault inside the mapping", m.fault, 0);
    check_int("guard: one page committed", (long)m.committed, 4096);

    /* The page below the base is the guard: any access is a fault, and it
     * is flagged as a guard (stack overflow), not an ordinary unmapped hit. */
    m.fault = 0;
    m.fault_guard = 0;
    check_true("guard: access below base returns NULL",
               gm_page(&m, base - 1, 0) == NULL);
    check_int("guard: fault recorded", m.fault, 1);
    check_int("guard: flagged as a guard page", m.fault_guard, 1);
    check_int("guard: fault address is the touched byte",
              (long)m.fault_addr, (long)(base - 1));
    check_str("guard: reason", m.fault_why, "stack overflow");

    gm_free(&m);
}

/****************************************************************
 * gm_munmap frees the mapping's committed pages and drops both the
 * mapping and its guard region, so the base faults as unmapped after.
 ****************************************************************/

static void
test_munmap(void)
{
    guest_mem m;
    uint32_t base;

    gm_init(&m, LIMIT);
    base = gm_mmap(&m, 8192, GM_MAP_GUARD_LO);
    check_true("munmap: mmap succeeded", base != 0);

    gm_write8(&m, base, 1);             /* commit one page of the mapping */
    check_int("munmap: committed before", (long)m.committed, 4096);

    gm_munmap(&m, base, 8192);
    check_int("munmap: committed returned", (long)m.committed, 0);

    /* Both the mapping and its guard are gone: an access is now unmapped,
     * not a guard fault. */
    m.fault = 0;
    m.fault_guard = 0;
    check_true("munmap: base now unmapped", gm_page(&m, base, 0) == NULL);
    check_int("munmap: not a guard fault", m.fault_guard, 0);
    check_str("munmap: reason", m.fault_why, "unmapped");

    gm_free(&m);
}

/****************************************************************
 * The aggregate pool caps the total committed across guests. When the
 * pool is full a commit faults as "memory pool exhausted", even though
 * the guest's own limit is not reached.
 ****************************************************************/

static void
test_pool(void)
{
    guest_mem m;
    struct gm_pool pool;
    uint32_t base;

    memset(&pool, 0, sizeof(pool));
    pool.limit = 8192;                  /* two pages across all guests */

    gm_init(&m, LIMIT);                 /* a generous per-guest limit */
    m.pool = &pool;

    base = gm_mmap(&m, 3 * 4096, 0);    /* three pages of address space */
    check_true("pool: mmap succeeded", base != 0);

    gm_write8(&m, base + 0 * 4096, 1);  /* first two pages fit the pool */
    gm_write8(&m, base + 1 * 4096, 1);
    check_int("pool: two pages accounted", (long)pool.used, 8192);
    check_int("pool: no fault yet", m.fault, 0);

    gm_write8(&m, base + 2 * 4096, 1);  /* the third exceeds the pool */
    check_int("pool: third page faults", m.fault, 1);
    check_str("pool: reason", m.fault_why, "memory pool exhausted");
    check_int("pool: still only two pages used", (long)pool.used, 8192);

    gm_free(&m);                        /* returns this guest's pages to the pool */
    check_int("pool: freed back to the pool", (long)pool.used, 0);
}

/****************************************************************
 * gm_read_out copies a guest range into a host buffer.
 ****************************************************************/

static void
test_read_out(void)
{
    guest_mem m;
    uint32_t base;
    uint8_t buf[4];
    int i;

    gm_init(&m, LIMIT);
    base = gm_mmap(&m, 4096, 0);
    check_true("read_out: mmap succeeded", base != 0);

    for (i = 0; i < 4; i++)
        gm_write8(&m, base + i, (uint8_t)(0x10 + i));

    memset(buf, 0, sizeof(buf));
    gm_read_out(&m, base, buf, 4);
    check_int("read_out: byte 0", buf[0], 0x10);
    check_int("read_out: byte 1", buf[1], 0x11);
    check_int("read_out: byte 2", buf[2], 0x12);
    check_int("read_out: byte 3", buf[3], 0x13);

    gm_free(&m);
}

/****************************************************************
 * A mapping whose extent would leave the 32-bit space is refused rather than
 * wrapping the cursor back to a low address that overlaps the image, the heap,
 * or an earlier mapping. gm_munmap likewise ignores a wrapping range.
 ****************************************************************/

static void
test_overflow(void)
{
    guest_mem m;
    uint32_t base;
    int before;

    gm_init(&m, (uint64_t)4 << 30);     /* a limit large enough not to be the gate */

    /* A near-4GB request cannot fit below 0x100000000, so it is refused and
     * nothing is mapped (no wrap to a low, overlapping base). */
    before = m.nregions;
    base = gm_mmap(&m, 0xFFFFF000u, 0);
    check_int("overflow: huge mmap refused", base, 0);
    check_int("overflow: no region added", m.nregions, before);

    /* Place the cursor near the top, then a page-plus-guard request whose
     * trailing gap crosses the boundary is refused, and the guard is not left
     * mapped without its mapping (slots reserved up front). */
    m.mmap_next = 0xFFFFE000u;
    before = m.nregions;
    base = gm_mmap(&m, 4096, GM_MAP_GUARD_LO);
    check_int("overflow: boundary mmap refused", base, 0);
    check_int("overflow: no dangling guard region", m.nregions, before);

    /* A normal mapping still works once the cursor is sane again. */
    m.mmap_next = 0;
    base = gm_mmap(&m, 4096, 0);
    check_true("overflow: normal mmap still works", base != 0);

    /* gm_munmap on a range that wraps the 32-bit space is a no-op: it must not
     * walk a page count derived from a wrapped end and touch unrelated pages. */
    gm_write8(&m, base, 1);
    check_int("overflow: one page committed", (long)m.committed, 4096);
    gm_munmap(&m, 0xFFFFF000u, 8192);   /* base + size wraps past 4GB */
    check_int("overflow: wrapping munmap left committed alone",
              (long)m.committed, 4096);
    check_int("overflow: no fault from the wrapping munmap", m.fault, 0);

    gm_free(&m);
}

int
main(void)
{
    test_guard();
    test_munmap();
    test_pool();
    test_read_out();
    test_overflow();

    if (failures) {
        printf("test_guest_mmap: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_guest_mmap: all guest-mapping checks passed\n");
    return 0;
}
